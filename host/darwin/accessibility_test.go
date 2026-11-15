//go:build darwin && !ios

package darwin

import (
	"testing"

	. "github.com/egoist/asuka/internal/darwin"
	"github.com/egoist/asuka/ops"
	"github.com/egoist/asuka/signal"
	"github.com/egoist/asuka/ui"
)

func TestControlLabels(t *testing.T) {
	label := signal.New("Name")
	text := signal.New("Ada")
	on := signal.New(false)
	h, r := mount(t, func() ui.Node {
		return ui.Column(ui.Style{},
			ui.TextField(ui.TextFieldProps{Value: text, Label: label}),
			ui.TextField(ui.TextFieldProps{Value: text, Label: label, Multiline: true}),
			ui.TextField(ui.TextFieldProps{Rich: signal.NewFunc(ui.Rich{ui.Strong("Ada")}, nil), Label: label}),
			ui.Toggle(ui.ToggleProps{Value: on, Label: label}),
			ui.Toggle(ui.ToggleProps{Value: on, Label: label, Checkbox: true}),
		)
	})
	onMain(func() {
		check := func(want string) {
			t.Helper()
			for _, kind := range []ops.Kind{ops.KindTextField, ops.KindToggle} {
				nodes := find(h, kind)
				count := 2
				if kind == ops.KindTextField {
					count = 3
				}
				if len(nodes) != count {
					fatalf(t, "%v: %d controls, want %d", kind, len(nodes), count)
				}
				for _, n := range nodes {
					v := n.view
					if kind == ops.KindTextField && n.tracking != 0 {
						v = n.tracking
					}
					if got := GoString(Send(v, "accessibilityLabel")); got != want {
						t.Errorf("%v (multiline %t): label %q, want %q", kind, n.tracking != 0, got, want)
					}
				}
			}
		}
		check("Name")
		h.enter(func() { text.Set("Grace"); on.Set(true); r.Flush() })
		check("Name")
		for _, name := range []string{"Full name", ""} {
			h.enter(func() { label.Set(name); r.Flush() })
			check(name)
		}
	})
}
