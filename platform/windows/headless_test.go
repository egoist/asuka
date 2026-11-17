package windows_test

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"testing"
)

// RFC 0010's host contract (testdata/headless.asuka), driven as a user
// would:
//
//   - Tab moves the focus among the focusable views in the views' order,
//     visibly (FocusChange 2: focusVisible), and FocusChange 0 as it goes;
//     the Fluent focus rect is drawn around a focused view, and not around
//     one with NoFocusRing; text typed goes to the focused view's KeyText.
//   - In a FocusTrap, Tab and Shift+Tab cycle within it.
//   - A press focuses a focusable view, not visibly (FocusChange 1), and
//     reports where it began (PressPoint) first.
//   - An Overlay is placed over its root, next to its anchor (below it),
//     over the root and the overlays presented before it; its anchor is
//     followed as a scroll view around it scrolls (AnchorFrame).
//   - A press outside an overlay (as Windows hit-tests it) is reported to
//     it (OutsidePress) and, not modal, reaches what's under it; modal,
//     the overlay takes it and what's behind gets nothing. Escape closes
//     one (the root's keys, the core's).
func TestHeadless(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	src, err := os.ReadFile(filepath.Join("testdata", "headless.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	buildApp(t, w, string(src), local)
	d := script(t, w, local, guest, "app.exe",
		"snap:start",
		"key:tab", "snap:tab1",
		"key:tab", "snap:tab2",
		"type:ba", "snap:typed",
		"key:tab", "snap:trap1",
		"key:tab", "snap:trap2",
		"key:tab", "snap:trap3",
		"key:shift+tab", "snap:trap4",
		"tap:Ringed", "snap:pressed",
		"tap:Point", "snap:point",
		"tap:Choose", "snap:menu",
		"click:Elsewhere", "snap:outside",
		"tap:Choose", "key:escape", "snap:escaped",
		"tap:Deep", "snap:tip",
		"scroll:30", "snap:scrolled",
		"tap:Show", "snap:dialog",
		"click:Behind", "snap:modal",
	)
	shows(t, d["start"], "the start", []string{"Ringed", "Plain", "One", "Two", "Log:"})
	shows(t, d["tab1"], "Tab", []string{"Ringed visible", "Log: r:true"})
	shows(t, d["tab2"], "Tab again", []string{"Ringed", "Plain focused", "Log: r:true r:false"})
	shows(t, d["typed"], "typed", []string{"Typed:ba"})
	shows(t, d["trap1"], "into the trap", []string{"One*", "Two"})
	shows(t, d["trap2"], "in the trap", []string{"One", "Two*"})
	shows(t, d["trap3"], "the trap's cycle", []string{"One*", "Two"}, "Ringed visible", "Plain focused")
	shows(t, d["trap4"], "the trap's cycle back", []string{"One", "Two*"}, "Ringed visible", "Plain focused")
	shows(t, d["pressed"], "a press", []string{"Ringed focused", "One", "Two"})
	if !regexp.MustCompile(`"At:50,(9|10)"`).MatchString(d["point"]) {
		t.Errorf("a press's point isn't the middle of Point (100×19):\n%s", d["point"])
	}

	// The Fluent focus rect, at the focused view's top edge (Ringed at 16,
	// 16, Plain at 144, 16: 120×32 points, pixels at 96 DPI): drawn for
	// Ringed, not for Plain (NoFocusRing).
	yellow := func(r, g, b int) bool { return r > 230 && g > 180 && g < 230 && b < 60 }
	if r, g, b := picture(t, local, "tab1").at(76, 16); yellow(r, g, b) {
		t.Errorf("no focus rect around the focused view: %d %d %d", r, g, b)
	}
	tab2 := picture(t, local, "tab2")
	if r, g, b := tab2.at(204, 16); !yellow(r, g, b) {
		t.Errorf("a focus rect around a view with NoFocusRing: %d %d %d", r, g, b)
	}
	if r, g, b := tab2.at(76, 16); !yellow(r, g, b) {
		t.Errorf("the focus rect stayed: %d %d %d", r, g, b)
	}

	// The menu: over the root, below its anchor (Choose, 20 points tall,
	// offset 4).
	menu := overlays(t, d["menu"])
	if len(menu) != 1 || !menu[0].over || menu[0].frame[1] < anchorBottom(t, d["menu"], "Choose")+4 {
		t.Errorf("the menu isn't over the root below Choose: %+v\n%s", menu, d["menu"])
	}
	shows(t, d["menu"], "the menu", []string{"Apple", "Pear"})
	shows(t, d["outside"], "a press outside the menu", []string{"Closed by outsidePress", "Picked elsewhere"}, "Apple")
	shows(t, d["escaped"], "Escape", []string{"Closed by escape"}, "Apple")

	// The tip follows Deep as its scroll view scrolls.
	tip, scrolled := overlays(t, d["tip"]), overlays(t, d["scrolled"])
	if len(tip) != 1 || len(scrolled) != 1 {
		t.Fatalf("no tip:\n%s\n%s", d["tip"], d["scrolled"])
	}
	if dy := scrolled[0].frame[1] - tip[0].frame[1]; dy > -29 || dy < -31 {
		t.Errorf("scrolled 30 points, the tip moved %v:\n%s\n%s", dy, d["tip"], d["scrolled"])
	}

	// The dialog: modal, on top of the tip, presented before it.
	dlg := overlays(t, d["dialog"])
	if len(dlg) != 2 {
		t.Fatalf("not the tip and the dialog:\n%s", d["dialog"])
	}
	for _, o := range dlg {
		if !o.over {
			t.Errorf("an overlay under its root: %+v", o)
		}
		if o.modal != (o.z == 0) {
			t.Errorf("the modal dialog isn't the topmost overlay: %+v", dlg)
		}
	}
	shows(t, d["modal"], "a press behind the modal dialog", []string{"Why outsidePress"}, "Sure?")
	if strings.Contains(d["modal"], `"Why pressed behind"`) {
		t.Errorf("the press went through the modal dialog:\n%s", d["modal"])
	}
}

type overlayDump struct {
	seq, z      int
	over, modal bool
	frame       [4]float64 // its first child's: x, y, w, h in the root
}

var (
	overlayLine = regexp.MustCompile(`^overlay (\d+) z(\d+)( over)?( modal)?:$`)
	frameRe     = regexp.MustCompile(`\[(-?[\d.]+),(-?[\d.]+) (-?[\d.]+)x(-?[\d.]+)\]`)
)

// overlays reads the overlays a dump shows: each its place in the z-order
// and its content's frame (the first view in it).
func overlays(t *testing.T, dump string) []overlayDump {
	t.Helper()
	var out []overlayDump
	lines := strings.Split(dump, "\n")
	for i, l := range lines {
		m := overlayLine.FindStringSubmatch(l)
		if m == nil {
			continue
		}
		o := overlayDump{over: m[3] != "", modal: m[4] != ""}
		o.seq, _ = strconv.Atoi(m[1])
		o.z, _ = strconv.Atoi(m[2])
		for _, c := range lines[i+1:] {
			if !strings.HasPrefix(c, "    ") {
				if !strings.HasPrefix(c, "  ") {
					break
				}
				continue
			}
			if f := frameRe.FindStringSubmatch(c); f != nil {
				for j := range 4 {
					o.frame[j], _ = strconv.ParseFloat(f[j+1], 64)
				}
			}
			break
		}
		out = append(out, o)
	}
	return out
}

// anchorBottom is the bottom of the view around the text (its line's
// parent's frame, in the root: the root's children's frames are).
func anchorBottom(t *testing.T, dump, text string) float64 {
	t.Helper()
	lines := strings.Split(dump, "\n")
	for i, l := range lines {
		if !strings.Contains(l, `"`+text+`"`) {
			continue
		}
		// The view it's in, then the row that's in: frames add up to the
		// root's coordinates.
		depth := len(l) - len(strings.TrimLeft(l, " "))
		var y, h float64
		first := true
		for j := i - 1; j >= 0 && depth > 2; j-- {
			d := len(lines[j]) - len(strings.TrimLeft(lines[j], " "))
			if d >= depth {
				continue
			}
			depth = d
			f := frameRe.FindStringSubmatch(lines[j])
			if f == nil {
				break
			}
			fy, _ := strconv.ParseFloat(f[2], 64)
			y += fy
			if first {
				h, _ = strconv.ParseFloat(f[4], 64)
				first = false
			}
		}
		return y + h
	}
	t.Fatalf("no %q:\n%s", text, dump)
	return 0
}

// Screen readers' roles, states and relations (testdata/a11ystates.asuka),
// read by UI Automation's client in the VM: a checkbox's Toggle state, a
// combobox's ExpandCollapse state and the listbox it controls, a slider's
// value and range, a selected tab, a label's and a description's
// relations (the name and help text they give), a dialog; expanding the
// combobox presses it, showing the listbox it controls; toggling the
// checkbox shows a modal view, which hides the rest of the window from
// the control view.
func TestHeadlessAccessibility(t *testing.T) {
	w := onWindows(t)
	if w.vm == "" && os.Getenv("ASUKA_UIA") == "" {
		t.Skip("reads the desktop's UI Automation tree: in the VM, or with ASUKA_UIA set")
	}
	local, guest := w.job(t)
	src, err := os.ReadFile(filepath.Join("testdata", "a11ystates.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	buildApp(t, w, string(src), local)
	if err := os.WriteFile(filepath.Join(local, "uia.ps1"), []byte(statesScript), 0o644); err != nil {
		t.Fatal(err)
	}
	buildProbe(t, w, local)
	out, err := w.run(t, guest, `C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe`, map[string]string{
		"ASUKA_SCRIPT":   "waitfor:" + guest + `\uia.done`,
		"ASUKA_SNAPSHOT": guest + `\end.png`,
		"ASUKA_DUMP":     "1",
	}, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", guest+`\uia.ps1`, guest)
	if err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	t.Logf("%s", out)
	before, after, _ := strings.Cut(out, "--- expanded")
	after, modal, _ := strings.Cut(after, "--- modal")
	for _, want := range []string{
		"CheckBox|Agree|Must be checked",
		"toggle Off",
		"labeledby Agree",
		"describedby Must be checked",
		"ComboBox|Combo|",
		"expand Collapsed",
		"Slider|Volume|",
		"value 40%",
		"range 40 0 100 True",
		"TabItem|Mail|",
		"selected True",
		"Pane|Settings|",
		"dialog True",
	} {
		if !strings.Contains(before, want) {
			t.Errorf("no %q in the tree", want)
		}
	}
	for _, want := range []string{"expand Expanded", "controllerfor Items", "List|Items|"} {
		if !strings.Contains(after, want) {
			t.Errorf("expanded: no %q in the tree", want)
		}
	}
	if !strings.Contains(modal, "Pane|Done|") || !strings.Contains(modal, "Text|Thanks|") {
		t.Errorf("no modal dialog once ticked:\n%s", modal)
	}
	if strings.Contains(modal, "ComboBox|Combo") || strings.Contains(modal, "Slider|Volume") || strings.Contains(modal, "Text|Agree") {
		t.Errorf("the modal view doesn't hide the rest:\n%s", modal)
	}
}

// buildProbe builds probe.exe, a UI Automation client in C (the native
// IUIAutomation: the relations .NET's client has no properties for), into
// dir: probe.exe <window> <name> <control type> <property> <label> finds
// the element of that name and control type in the window and prints the
// label and the name of each element the property names.
func buildProbe(t *testing.T, w *windows, dir string) {
	t.Helper()
	zig(t)
	src := filepath.Join(dir, "probe.c")
	if err := os.WriteFile(src, []byte(probeC), 0o644); err != nil {
		t.Fatal(err)
	}
	arch := map[string]string{"arm64": "aarch64", "amd64": "x86_64"}[w.arch]
	cc := []string{"cc", "-target", arch + "-windows-gnu"}
	if runtime.GOOS == "windows" && os.Getenv("ASUKA_CC") != "zig" {
		cc = []string{}
	}
	args := append(cc, "-std=c17", "-Wall", "-Werror", src, "-o", filepath.Join(dir, "probe.exe"), "-lole32", "-loleaut32")
	name := "zig"
	if len(cc) == 0 {
		name = "cc"
	}
	if out, err := exec.Command(name, args...).CombinedOutput(); err != nil {
		t.Fatalf("building the probe: %v\n%s", err, out)
	}
}

const probeC = `#define COBJMACROS
#include <windows.h>
#include <uiautomation.h>
#include <stdio.h>
#include <stdlib.h>

static const GUID clsid_uia = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
static const GUID iid_uia = {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};
static const GUID iid_element = {0xd22108aa, 0x8ac5, 0x49a5, {0x83, 0x7b, 0x37, 0xbb, 0xb3, 0xd7, 0x59, 0x1e}};

static void print_name(const char *label, IUnknown *u) {
	IUIAutomationElement *e = NULL;
	if (!u || FAILED(IUnknown_QueryInterface(u, &iid_element, (void **)&e)) || !e) return;
	BSTR name = NULL;
	IUIAutomationElement_get_CurrentName(e, &name);
	printf("%s %ls\n", label, name ? name : L"");
	SysFreeString(name);
	IUIAutomationElement_Release(e);
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc < 6) {
		printf("probe: %d arguments\n", argc);
		return 2;
	}
	HWND hwnd = (HWND)(intptr_t)_strtoi64(argv[1], NULL, 10);
	wchar_t name[256];
	MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, name, 256);
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	IUIAutomation *uia = NULL;
	HRESULT hr = CoCreateInstance(&clsid_uia, NULL, CLSCTX_INPROC_SERVER, &iid_uia, (void **)&uia);
	if (FAILED(hr)) {
		printf("%s: no UI Automation (%08lx)\n", argv[5], (unsigned long)hr);
		return 3;
	}
	IUIAutomationElement *root = NULL, *found = NULL;
	hr = IUIAutomation_ElementFromHandle(uia, hwnd, &root);
	if (FAILED(hr) || !root) {
		printf("%s: no window %s (%08lx)\n", argv[5], argv[1], (unsigned long)hr);
		return 4;
	}
	VARIANT vn, vt;
	VariantInit(&vn);
	VariantInit(&vt);
	vn.vt = VT_BSTR, vn.bstrVal = SysAllocString(name);
	vt.vt = VT_I4, vt.lVal = atoi(argv[3]);
	IUIAutomationCondition *cn = NULL, *ct = NULL, *both = NULL;
	IUIAutomation_CreatePropertyCondition(uia, UIA_NamePropertyId, vn, &cn);
	IUIAutomation_CreatePropertyCondition(uia, UIA_ControlTypePropertyId, vt, &ct);
	IUIAutomation_CreateAndCondition(uia, cn, ct, &both);
	IUIAutomationElement_FindFirst(root, TreeScope_Descendants, both, &found);
	if (!found) {
		printf("%s: no %s\n", argv[5], argv[2]);
		return 0;
	}
	// The relation, as the client's own getter reads it.
	IUIAutomationElementArray *a = NULL;
	if (atoi(argv[4]) == UIA_ControllerForPropertyId) IUIAutomationElement_get_CurrentControllerFor(found, &a);
	else IUIAutomationElement_get_CurrentDescribedBy(found, &a);
	int len = 0;
	if (a) IUIAutomationElementArray_get_Length(a, &len);
	if (!len) printf("%s: none\n", argv[5]);
	for (int i = 0; i < len; i++) {
		IUIAutomationElement *e = NULL;
		if (SUCCEEDED(IUIAutomationElementArray_GetElement(a, i, &e)) && e) {
			print_name(argv[5], (IUnknown *)e);
			IUIAutomationElement_Release(e);
		}
	}
	if (a) IUIAutomationElementArray_Release(a);
	return 0;
}
`

// statesScript starts the app (args[0]: its job), prints each element of
// its window (control type|name|help text) and its states and relations,
// expands the combobox (ExpandCollapse) and prints them again, then
// toggles the checkbox (Toggle) and prints them again.
const statesScript = `
param([string]$dir)
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$p = Start-Process -FilePath "$dir\app.exe" -WorkingDirectory $dir -PassThru -RedirectStandardOutput "$dir\app.txt" -RedirectStandardError "$dir\app.err.txt" -WindowStyle Normal
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 100 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $p.Refresh(); $h = $p.MainWindowHandle }
if ($h -eq [IntPtr]::Zero) { "no window"; Set-Content "$dir\uia.done" "x"; $p.WaitForExit(); exit 1 }
Start-Sleep -Milliseconds 1500
$A = [System.Windows.Automation.AutomationElement]
function Dump {
  $root = $A::FromHandle($h)
  $all = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
  foreach ($e in $all) {
    $c = $e.Current
    $type = $c.ControlType.ProgrammaticName -replace '^ControlType\.', ''
    "$type|$($c.Name)|$($c.HelpText)"
    $o = $null
    if ($e.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$o)) { "toggle $($o.Current.ToggleState)" }
    if ($e.TryGetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$o)) { "expand $($o.Current.ExpandCollapseState)" }
    if ($e.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$o)) { "selected $($o.Current.IsSelected)" }
    if ($e.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$o)) { "value $($o.Current.Value)" }
    if ($e.TryGetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern, [ref]$o)) { $v = $o.Current; "range $($v.Value) $($v.Minimum) $($v.Maximum) $($v.IsReadOnly)" }
    if ($c.LabeledBy) { "labeledby $($c.LabeledBy.Current.Name)" }
    $dl = $e.GetCurrentPropertyValue([System.Windows.Automation.AutomationProperty]::LookupById(30174))
    if ($dl -eq $true) { "dialog True" }
  }
}
Dump
# ControllerFor and DescribedBy, which .NET's client doesn't know: the
# native client's (probe.exe).
& "$dir\probe.exe" "$h" "Agree" 50002 30105 describedby 2>&1
"probe $LASTEXITCODE"
$root = $A::FromHandle($h)
$combo = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, (New-Object System.Windows.Automation.PropertyCondition($A::NameProperty, "Combo")))
$x = $null
if ($combo -and $combo.TryGetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$x)) { $x.Expand(); "expanded it" }
Start-Sleep -Milliseconds 1200
"--- expanded"
Dump
& "$dir\probe.exe" "$h" "Combo" 50003 30104 controllerfor 2>&1
"probe $LASTEXITCODE"
$all = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, (New-Object System.Windows.Automation.PropertyCondition($A::ControlTypeProperty, [System.Windows.Automation.ControlType]::CheckBox)))
foreach ($b in $all) { if ($b.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$x)) { $x.Toggle(); "toggled it" } }
Start-Sleep -Milliseconds 1200
"--- modal"
Dump
Set-Content "$dir\uia.done" "x"
$p.WaitForExit()
Get-Content "$dir\app.err.txt"
exit 0
`
