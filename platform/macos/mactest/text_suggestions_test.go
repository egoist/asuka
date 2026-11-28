//go:build darwin

package mactest

import (
	"slices"
	"testing"
)

// suggestOf is a field's suggestions' state (the host's "suggestions"
// JSON).
func suggestOf(t *testing.T, tr *Tree, placeholder string) (*Node, map[string]any) {
	t.Helper()
	for _, n := range tr.Kind("TextField") {
		if n.Str("placeholder") == placeholder {
			return n, vobj(n, "suggestions")
		}
	}
	t.Fatalf("no field %q:\n%s", placeholder, tr.Raw())
	return nil, nil
}

// A field with suggestions shows them in a window under it once the user
// types (as the core filtered them), Down and Return choose one (its text
// in the field, reported with Suggestion, not TextChanged), Escape closes
// the list, and losing the focus closes it.
func TestTextSuggestions(t *testing.T) {
	a := Run(t, `
		component S {
		  state city = ""
		  state chosen = "-"
		  state changes = 0
		  Column {
		    padding: 16; gap: 8
		    TextField {
		      text: city
		      on change(t) { city = t; changes += 1 }
		      placeholder: "Destination"
		      suggestions: ["Amsterdam", "Athens", "Barcelona", "Berlin", "Paris"]
		      on suggestion(i) { chosen = "{i}" }
		    }
		    Text("log:{city}|{chosen}|{changes}")
		  }
		}
		app { S() }
	`)
	tr := a.UntilText("log:|-|0")
	f, s := suggestOf(t, tr, "Destination")
	if f.Class != "AsukaTextField" || vbool(s, "shown") || vbool(s, "dropdown") {
		t.Fatalf("field %s, suggestions %v", f.Class, s)
	}
	// Focus alone shows nothing.
	a.Do("tap Destination")
	tr = a.Tree()
	if _, s := suggestOf(t, tr, "Destination"); vbool(s, "shown") {
		t.Errorf("shown on focus: %v", s)
	}
	a.Do("type a")
	tr = a.UntilText("log:a|-|1")
	tr = a.Until(func(tr *Tree) bool { _, s := suggestOf(t, tr, "Destination"); return vbool(s, "shown") })
	_, s = suggestOf(t, tr, "Destination")
	if items := vstrs(s, "items"); !slices.Equal(items, []string{"Amsterdam", "Athens", "Barcelona", "Paris"}) {
		t.Errorf("items %q", items)
	}
	if vnum(s, "rows") != 4 || vnum(s, "selected") != -1 {
		t.Errorf("list %v", s)
	}
	a.Do("type t")
	tr = a.UntilText("log:at|-|2")
	tr = a.Until(func(tr *Tree) bool { _, s := suggestOf(t, tr, "Destination"); return vnum(s, "rows") == 1 })
	_, s = suggestOf(t, tr, "Destination")
	if items := vstrs(s, "items"); !slices.Equal(items, []string{"Athens"}) {
		t.Errorf("items after at: %q", items)
	}
	// Down highlights the first, Return chooses it.
	a.Do("key down")
	a.Until(func(tr *Tree) bool { _, s := suggestOf(t, tr, "Destination"); return vnum(s, "selected") == 0 })
	a.Do("key enter")
	tr = a.UntilText("log:Athens|1|3") // its index among them all; the core reports the text as typed
	f, s = suggestOf(t, tr, "Destination")
	if vbool(s, "shown") || f.Str("stringValue") != "Athens" {
		t.Errorf("after the choice: %q %v", f.Str("stringValue"), s)
	}
	// Typing again shows the list again; Escape closes it.
	a.Do("key backspace")
	tr = a.UntilText("log:Athen|1|4")
	a.Until(func(tr *Tree) bool { _, s := suggestOf(t, tr, "Destination"); return vbool(s, "shown") })
	a.Do("escape")
	a.Until(func(tr *Tree) bool { _, s := suggestOf(t, tr, "Destination"); return !vbool(s, "shown") })
	// A click on a row.
	a.Do("key backspace")
	tr = a.UntilText("log:Athe|1|5")
	a.Until(func(tr *Tree) bool { _, s := suggestOf(t, tr, "Destination"); return vbool(s, "shown") })
	f, _ = suggestOf(t, tr, "Destination")
	a.Do("suggestpick " + vid(f) + " 0")
	a.UntilText("log:Athens|1|6")
}

// A Dropdown field is an NSComboBox: its list (its own pop-up) opens as
// the user types, and its item chosen is reported with Suggestion.
func TestTextSuggestionsDropdown(t *testing.T) {
	a := Run(t, `
		component S {
		  state city = ""
		  state chosen = "-"
		  Column {
		    padding: 16; gap: 8
		    TextField {
		      bind text: city
		      placeholder: "City"
		      suggestions: ["Kyoto", "Osaka", "Tokyo"]
		      dropdown: true
		      on suggestion(i) { chosen = "{i}" }
		    }
		    Text("log:{city}|{chosen}")
		  }
		}
		app { S() }
	`)
	tr := a.UntilText("log:|-")
	f, s := suggestOf(t, tr, "City")
	if f.Class != "AsukaComboBox" || !vbool(s, "combo") || vnum(s, "count") != 3 {
		t.Fatalf("field %s, suggestions %v", f.Class, s)
	}
	a.Do("tap City")
	a.Do("type o")
	tr = a.UntilText("log:o|-")
	tr = a.Until(func(tr *Tree) bool { _, s := suggestOf(t, tr, "City"); return vbool(s, "shown") })
	f, s = suggestOf(t, tr, "City")
	if items := vstrs(s, "items"); !slices.Equal(items, []string{"Kyoto", "Osaka", "Tokyo"}) {
		t.Errorf("items %q", items)
	}
	a.Do("suggestpick " + vid(f) + " 1")
	tr = a.UntilText("log:Osaka|1")
	if f, _ := suggestOf(t, tr, "City"); f.Str("stringValue") != "Osaka" {
		t.Errorf("field shows %q", f.Str("stringValue"))
	}
}
