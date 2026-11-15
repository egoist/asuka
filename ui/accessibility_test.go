package ui_test

import (
	"testing"

	"github.com/egoist/asuka/ops"
	"github.com/egoist/asuka/signal"
	"github.com/egoist/asuka/ui"
)

// Naming a control must not replace its value, and the name follows the
// language just as a button's label does.
func TestControlLabelsFollow(t *testing.T) {
	label := signal.Func(func() string { return say("hello") })
	text := signal.New("Ada")
	on := signal.New(true)
	h, r := mountLocale(t, "en", func() ui.Node {
		return ui.Column(ui.Style{},
			ui.TextField(ui.TextFieldProps{Value: text, Label: label}),
			ui.TextField(ui.TextFieldProps{Value: text, Label: label, Multiline: true}),
			ui.Toggle(ui.ToggleProps{Value: on, Label: label}),
			ui.Toggle(ui.ToggleProps{Value: on, Label: label, Checkbox: true}),
		)
	})
	check := func(want string) {
		t.Helper()
		for _, kind := range []ops.Kind{ops.KindTextField, ops.KindToggle} {
			views := h.OfKind(kind)
			if len(views) != 2 {
				t.Fatalf("%v: %d controls, want 2", kind, len(views))
			}
			for _, v := range views {
				if got := v.String(ops.PropA11yLabel); got != want {
					t.Errorf("%v: label %q, want %q", kind, got, want)
				}
			}
		}
	}
	check("Hello")
	text.Set("Grace")
	on.Set(false)
	r.Flush()
	check("Hello")
	ui.SetLocale("de")
	r.Flush()
	check("Hallo")
	for _, v := range h.OfKind(ops.KindTextField) {
		if got := v.String(ops.PropTextFieldValue); got != "Grace" {
			t.Errorf("field value %q after renaming", got)
		}
	}
	for _, v := range h.OfKind(ops.KindToggle) {
		if v.Bool(ops.PropToggleOn) {
			t.Error("toggle value changed with its label")
		}
	}
	r.Dispose()
	h.Reset()
	ui.SetLocale("en")
	r.Flush()
	if h.Count(ops.OpSetProp) != 0 {
		t.Fatal("disposed labels still emit props")
	}
}
