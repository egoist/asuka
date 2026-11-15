//go:build windows

package windows

import (
	"syscall"
	"testing"

	. "github.com/egoist/asuka/internal/windows"
	"github.com/egoist/asuka/ops"
	"github.com/egoist/asuka/signal"
	"github.com/egoist/asuka/ui"
)

// Read the common control's MSAA provider, which also supplies its UIA
// proxy. Reading accessible.Property would only test the host's own views.
func nativeControlAccessibility(hwnd uintptr) (name, value string, role, state int32) {
	iid := NewGUID("618736e0-3c3d-11cf-810c-00aa00389b71") // IAccessible
	var acc uintptr
	proc := syscall.NewLazyDLL("oleacc.dll").NewProc("AccessibleObjectFromWindow")
	hr, _, _ := proc.Call(hwnd, 0xFFFFFFFC, Ptr(&iid), Ptr(&acc)) // OBJID_CLIENT
	if Failed(hr) || acc == 0 {
		fatalf("AccessibleObjectFromWindow: %#x", hr)
	}
	defer Release(acc)
	// VARIANT is 24 bytes on both supported Windows architectures. The
	// by-value child argument is passed indirectly; VT_I4, CHILDID_SELF.
	self := [3]uint64{3, 0, 0}
	text := func(method int) string {
		var bstr uintptr
		if hr := ComCall(acc, method, Ptr(&self), Ptr(&bstr)); Failed(hr) {
			fatalf("IAccessible method %d: %#x", method, hr)
		}
		defer SysFreeString(bstr)
		return GoStr(bstr)
	}
	integer := func(method int) int32 {
		var v [3]uint64
		if hr := ComCall(acc, method, Ptr(&self), Ptr(&v)); Failed(hr) || v[0] != 3 {
			fatalf("IAccessible method %d: HRESULT %#x, VARIANT %v", method, hr, v)
		}
		return int32(v[1])
	}
	return text(10), text(11), integer(13), integer(14) // name, value, role, state
}

func TestTextFieldControlLabel(t *testing.T) {
	for _, mode := range []string{"single", "multiline", "rich"} {
		t.Run(mode, func(t *testing.T) {
			ending := ""
			if mode == "rich" {
				ending = "\r" // MSAA includes RichEdit's final paragraph mark.
			}
			label := signal.New("Account")
			value := signal.New("Field value")
			rich := signal.NewFunc(ui.Rich{ui.Strong("Field value")}, nil)
			field := func(label signal.Accessor[string]) ui.Node {
				p := ui.TextFieldProps{Value: value, Label: label, Placeholder: ui.Val("Hint only"), Multiline: mode == "multiline"}
				if mode == "rich" {
					p.Rich = rich
				}
				return ui.TextField(p)
			}
			// Nil retains the control's native name. Clearing must return to
			// that same name, including any platform-provided fallback.
			defaults, _ := mount(t, func() ui.Node { return field(nil) })
			var defaultName string
			onMain(t, func() {
				name, text, role, _ := nativeControlAccessibility(find(defaults, ops.KindTextField)[0].ctl)
				defaultName = name
				if text != "Field value"+ending || role != 0x2A { // ROLE_SYSTEM_TEXT
					t.Errorf("native field value %q, role %#x", text, role)
				}
			})
			h, r := mount(t, func() ui.Node { return field(label) })
			onMain(t, func() {
				n := find(h, ops.KindTextField)[0]
				check := func(wantName, wantValue string) {
					wantValue += ending
					name, text, role, _ := nativeControlAccessibility(n.ctl)
					if name != wantName || text != wantValue || role != 0x2A {
						t.Errorf("MSAA name %q, value %q, role %#x; want %q, %q, text", name, text, role, wantName, wantValue)
					}
				}
				check("Account", "Field value")
				h.enter(func() {
					label.Set("Renamed account")
					if mode == "rich" {
						rich.Set(ui.Rich{ui.Strong("Edited value")})
					} else {
						value.Set("Edited value")
					}
					r.Flush()
				})
				check("Renamed account", "Edited value")
				// Alignment recreates both EDIT and RICHEDIT50W, without a
				// second Label op. The new control must retain its name.
				old := n.ctl
				var b ops.Batch
				b.SetEnum(n.id, ops.PropTextAlign, int32(ops.TextAlignCenter))
				h.Apply(&b)
				if n.ctl == old {
					t.Error("alignment did not replace the control")
				}
				check("Renamed account", "Edited value")
				h.enter(func() { label.Set(""); r.Flush() })
				check(defaultName, "Edited value")
			})
		})
	}
}

func TestToggleControlLabel(t *testing.T) {
	for _, checkbox := range []bool{false, true} {
		name := "switch"
		if checkbox {
			name = "checkbox"
		}
		t.Run(name, func(t *testing.T) {
			label := signal.New("Notifications")
			value := signal.New(true)
			defaults, _ := mount(t, func() ui.Node {
				return ui.Toggle(ui.ToggleProps{Value: value, Checkbox: checkbox})
			})
			var defaultName string
			onMain(t, func() {
				defaultName, _, _, _ = nativeControlAccessibility(find(defaults, ops.KindToggle)[0].ctl)
			})
			h, r := mount(t, func() ui.Node {
				return ui.Toggle(ui.ToggleProps{Value: value, Label: label, Checkbox: checkbox})
			})
			onMain(t, func() {
				n := find(h, ops.KindToggle)[0]
				check := func(wantName string, checked bool) {
					name, _, role, state := nativeControlAccessibility(n.ctl)
					if name != wantName || role != 0x2C || (state&0x10 != 0) != checked { // ROLE_SYSTEM_CHECKBUTTON, STATE_SYSTEM_CHECKED
						t.Errorf("MSAA name %q, role %#x, state %#x; want %q, checkbutton, checked=%v", name, role, state, wantName, checked)
					}
				}
				check("Notifications", true)
				h.enter(func() { label.Set("Alerts"); value.Set(false); r.Flush() })
				check("Alerts", false)
				h.enter(func() { label.Set(""); r.Flush() })
				check(defaultName, false)
			})
		})
	}
}
