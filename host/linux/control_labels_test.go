//go:build linux && !android

package linux

import (
	"testing"

	. "github.com/egoist/asuka/internal/gtk"
	"github.com/egoist/asuka/ops"
	"github.com/egoist/asuka/signal"
	"github.com/egoist/asuka/ui"
)

// Read GTK's accessibility context on the control itself, not the host's
// cached label or the AsukaView around the control.
func wantControlLabel(t *testing.T, w Ptr, label string) {
	t.Helper()
	if label == "" {
		if Call(Sym("gtk_test_accessible_has_property"), w, a11yPropLabel) != 0 {
			t.Error("GTK still has an explicit accessible label")
		}
		return
	}
	str := CString(label)
	if mismatch := Call(Sym("gtk_test_accessible_check_property"), w, a11yPropLabel, Addr(str)); mismatch != 0 {
		defer Free(mismatch)
		t.Errorf("GTK accessible label = %s, want %q", GoString(mismatch), label)
	}
}

func wantControlRole(t *testing.T, w Ptr, nick string) {
	t.Helper()
	role, ok := EnumValue(Call(Sym("gtk_accessible_role_get_type")), nick)
	if !ok || int32(Call(Sym("gtk_accessible_get_accessible_role"), w)) != role {
		t.Errorf("GTK control no longer has role %q", nick)
	}
}

func TestTextFieldControlLabel(t *testing.T) {
	for _, mode := range []string{"single", "secure", "multiline", "rich"} {
		for _, labelled := range []bool{false, true} {
			name := mode + "/default"
			if labelled {
				name = mode + "/label"
			}
			t.Run(name, func(t *testing.T) {
				label := signal.New("Account")
				value := signal.New("Field value")
				rich := signal.NewFunc(ui.Rich{ui.Strong("Field value")}, nil)
				h, r := mount(t, func() ui.Node {
					p := ui.TextFieldProps{Value: value, Placeholder: ui.Val("Hint only"), Secure: mode == "secure", Multiline: mode == "multiline"}
					if labelled {
						p.Label = label
					}
					if mode == "rich" {
						p.Rich = rich
					}
					return ui.TextField(p)
				})
				onMain(func() {
					n := fieldNode(t, h)
					check := func(wantLabel, wantValue string) {
						w := n.field.editable
						if w == 0 {
							w = n.field.view
						}
						wantControlLabel(t, w, wantLabel)
						wantControlLabel(t, n.widget, "")
						wantControlRole(t, w, "text-box")
						if got := h.fieldText(n); got != wantValue {
							t.Errorf("GTK field text = %q, want %q", got, wantValue)
						}
					}
					if !labelled {
						check("", "Field value")
						return
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
					// Plain recreates the entry/text view, without a new Label op.
					var b ops.Batch
					b.SetBool(n.id, ops.PropTextFieldPlain, true)
					h.Apply(&b)
					check("Renamed account", "Edited value")
					h.enter(func() { label.Set(""); r.Flush() })
					check("", "Edited value")
				})
			})
		}
	}
}

func TestToggleControlLabel(t *testing.T) {
	for _, checkbox := range []bool{false, true} {
		name, role := "switch", "switch"
		if checkbox {
			name, role = "checkbox", "checkbox"
		}
		t.Run(name, func(t *testing.T) {
			label := signal.New("Notifications")
			value := signal.New(true)
			h, r := mount(t, func() ui.Node {
				return ui.Column(ui.Style{},
					ui.Toggle(ui.ToggleProps{Value: value, Checkbox: checkbox}),
					ui.Toggle(ui.ToggleProps{Value: value, Label: label, Checkbox: checkbox}),
				)
			})
			onMain(func() {
				var named *node
				for _, n := range find(h, ops.KindToggle) {
					if n.a11yLabel == "Notifications" {
						named = n
					} else {
						wantControlLabel(t, n.inner, "") // nil Label
						wantControlRole(t, n.inner, role)
					}
				}
				if named == nil {
					fatalf(t, "no labelled toggle")
				}
				check := func(want string, checked uintptr) {
					wantControlLabel(t, named.inner, want)
					wantControlLabel(t, named.widget, "")
					wantControlRole(t, named.inner, role)
					// GTK_ACCESSIBLE_STATE_CHECKED, GTK_ACCESSIBLE_TRISTATE_*.
					if mismatch := Call(Sym("gtk_test_accessible_check_state"), named.inner, 1, checked); mismatch != 0 {
						defer Free(mismatch)
						t.Errorf("GTK checked state = %s, want %d", GoString(mismatch), checked)
					}
				}
				check("Notifications", 1)
				h.enter(func() { label.Set("Alerts"); value.Set(false); r.Flush() })
				check("Alerts", 0)
				// Changing the control kind must retain its name and state,
				// even without a second Label op.
				old := named.inner
				var b ops.Batch
				b.SetBool(named.id, ops.PropToggleCheckbox, !checkbox)
				h.Apply(&b)
				if named.inner == old {
					t.Error("the toggle control was not replaced")
				}
				role = "checkbox"
				if checkbox {
					role = "switch"
				}
				check("Alerts", 0)
				h.enter(func() { label.Set(""); r.Flush() })
				check("", 0)
			})
		})
	}
}
