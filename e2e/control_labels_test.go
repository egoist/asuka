//go:build e2e

package e2e

import (
	"encoding/json"
	"fmt"
	"slices"
	"strings"
	"testing"
)

// The checkbox's name survives both a native tap and a value set by Go.
// Its visible text is different, so only the control's accessibility
// label can satisfy the check or receive the tap.
func TestControlLabelsIOS(t *testing.T) {
	s := iosApp(t, "e2e/testdata/control-labels", "dev.asuka.controllabels")
	check := func(label, values string) {
		t.Helper()
		waitFor(t, s, []string{"ready", "Notifications", label, "values: " + values})
	}
	check("Accept terms", "Ada false false")
	s.tap("Accept terms")
	check("Accept terms", "Ada false true")
	s.tap("Accept terms")
	check("Accept terms", "Ada false false")
	s.tap("Set values")
	check("Accept terms", "Grace true true")
	s.tap("Rename")
	check("Agree to terms", "Grace true true")
	s.tap("Agree to terms")
	check("Agree to terms", "Grace true false")
	s.tap("Notifications")
	check("Agree to terms", "Grace false false")
}

// Read Chrome's accessibility tree, not the wrapper's aria-label: the
// editor itself must be named, including the replacement rich editor.
func TestControlLabelsWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	url := webServer(t, "e2e/testdata/control-labels")
	c := launchCDP(t, browser)
	var created struct{ TargetID string }
	if err := c.call("", "Target.createTarget", map[string]any{"url": "about:blank"}, &created, ""); err != nil {
		t.Fatal(err)
	}
	page := c.attach(t, created.TargetID)
	var res json.RawMessage
	if err := c.call(page, "Page.navigate", map[string]any{"url": url}, &res, ""); err != nil {
		t.Fatal(err)
	}
	s := &webScreen{t: t, url: url}
	showsLine(t, s, "ready")
	check := func(names ...string) {
		t.Helper()
		slices.Sort(names)
		var got []string
		until(t, func() bool {
			var tree struct {
				Nodes []struct {
					Ignored    bool
					Role, Name struct{ Value string }
				}
			}
			if err := c.call(page, "Accessibility.getFullAXTree", map[string]any{}, &tree, ""); err != nil {
				t.Fatal(err)
			}
			got = nil
			for _, n := range tree.Nodes {
				if !n.Ignored && (n.Role.Value == "textbox" || n.Role.Value == "checkbox" || n.Role.Value == "switch") {
					got = append(got, n.Name.Value)
				}
			}
			slices.Sort(got)
			return slices.Equal(got, names)
		}, func() string { return fmt.Sprintf("control names: %q", got) })
	}
	check("Name", "Notes", "Formatted notes", "Notifications", "Accept terms")
	c.eval(t, page, `(() => { const f = document.querySelector("input.asuka-field"); f.focus(); f.select(); return "focused"; })()`)
	if err := c.call(page, "Input.insertText", map[string]any{"text": "Lin"}, &res, ""); err != nil {
		t.Fatal(err)
	}
	showsLine(t, s, "values: Lin false false")
	check("Name", "Notes", "Formatted notes", "Notifications", "Accept terms")
	s.tap("Set values")
	showsLine(t, s, "values: Grace true true")
	s.tap("Rename")
	check("Full name", "Comments", "Formatted comments", "Notifications", "Agree to terms")
	s.tap("Clear labels")
	check("Enter a name", "", "", "Notifications", "")
	showsLine(t, s, "values: Grace true true")
}

// TalkBack names an EditText by its hint: a content description on one
// hides what's typed in it, and Android's accessibility checks flag it.
// The automation's a11y reads each view's accessibility node, the one
// TalkBack gets (UiAutomator's dump has no hint).
func TestControlLabelsAndroid(t *testing.T) {
	s := androidApp(t, "e2e/testdata/control-labels", "dev.asuka.controllabels")
	waitFor(t, s, []string{"ready"})
	check := func(fields []string, controls ...string) {
		t.Helper()
		var gotFields, gotControls []string
		until(t, func() bool {
			gotFields, gotControls = nil, nil
			for _, line := range s.read("a11y", "asuka-a11y.txt") {
				f := strings.Split(line, "\t") // class, hint, content description
				if len(f) != 3 {
					continue
				}
				switch f[0] {
				case "android.widget.EditText":
					gotFields = append(gotFields, "hint="+f[1]+" desc="+f[2])
				case "android.widget.Switch", "android.widget.CheckBox":
					gotControls = append(gotControls, f[2])
				}
			}
			return slices.Equal(gotFields, fields) && slices.Equal(gotControls, controls)
		}, func() string { return fmt.Sprintf("fields: %q, controls: %q", gotFields, gotControls) })
	}
	check([]string{"hint=Name desc=", "hint=Notes desc=", "hint=Formatted notes desc="}, "Notifications", "Accept terms")
	s.tap("Rename")
	check([]string{"hint=Full name desc=", "hint=Comments desc=", "hint=Formatted comments desc="}, "Notifications", "Agree to terms")
	s.tap("Clear labels")
	check([]string{"hint=Enter a name desc=", "hint= desc=", "hint= desc="}, "Notifications", "")
}
