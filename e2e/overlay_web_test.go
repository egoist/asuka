//go:build e2e

package e2e

import (
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
	"testing"
)

// TestOverlayWeb runs e2e/testdata/overlayweb in headless Chrome, with
// DevTools' trusted mouse and key events: RFC 0010's host contract in a
// browser.
//
//   - An Overlay is a manual popover in the top layer over the root, as
//     big as it; its content is placed under its anchor (AnchorFrame), and
//     follows the anchor as the scroll view around it scrolls; scrolled
//     out of view, the anchor closes it (anchorHidden).
//   - A press outside it closes it (OutsidePress) and reaches what it
//     lands on; Escape (the root's keys) closes it.
//   - A modal one: the root is inert under it; a press on what's behind
//     reaches only its layer, which reports it (OutsidePress).
//   - Presented again, an overlay shows over the others; one over a
//     sheet is in the sheet's modal dialog, under its anchor there.
//   - Focus: a press focuses a focusable view (not visibly), Tab and
//     Shift+Tab move the focus (visibly), a character typed goes to
//     keyText, pressStart says where a press began; NoFocusRing draws no
//     outline; a focus trap keeps Tab inside it.
//   - Screen readers' states reach the elements' ARIA attributes.
func TestOverlayWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	pageURL := webServer(t, "e2e/testdata/overlayweb")
	c := launchCDP(t, browser)
	var created struct{ TargetID string }
	if err := c.call("", "Target.createTarget", map[string]any{"url": "about:blank"}, &created, ""); err != nil {
		t.Fatal(err)
	}
	page := c.attach(t, created.TargetID)
	var res json.RawMessage
	if err := c.call(page, "Page.navigate", map[string]any{"url": pageURL}, &res, ""); err != nil {
		t.Fatal(err)
	}
	s := &webScreen{t: t, url: pageURL}
	showsLine(t, s, "status: ready")

	// The page's helpers: the innermost view showing text, and its rectangle.
	const helpers = `window.__view = (t) => {
		let best = null;
		for (const el of document.querySelectorAll(".asuka-view, button")) {
			if (el.textContent.trim() !== t || !el.getClientRects().length) continue;
			if (!best || best.contains(el)) best = el;
		}
		return best;
	};
	window.__rect = (t) => { const el = __view(t); if (!el) return ""; const r = el.getBoundingClientRect(); return [r.left, r.top, r.width, r.height].map(Math.round).join(" "); };
	"ok"`
	c.eval(t, page, helpers)
	rect := func(text string) (x, y, w, h float64) {
		t.Helper()
		var r string
		until(t, func() bool { r = c.eval(t, page, `__rect(`+strconv.Quote(text)+`)`); return r != "" }, func() string { return "no view shows " + text })
		fmt.Sscan(r, &x, &y, &w, &h)
		return
	}
	click := func(x, y float64) {
		t.Helper()
		for _, typ := range []string{"mouseMoved", "mousePressed", "mouseReleased"} {
			p := map[string]any{"type": typ, "x": x, "y": y, "button": "left", "clickCount": 1}
			if typ == "mouseMoved" {
				p["button"] = "none"
			}
			if err := c.call(page, "Input.dispatchMouseEvent", p, &res, ""); err != nil {
				t.Fatal(err)
			}
		}
	}
	tapOn := func(text string) {
		t.Helper()
		x, y, w, h := rect(text)
		click(x+w/2, y+h/2)
	}
	type key struct {
		key, code string
		vk        int
		text      string
		mods      int
	}
	press := func(k key) {
		t.Helper()
		down := map[string]any{"type": "rawKeyDown", "key": k.key, "code": k.code, "windowsVirtualKeyCode": k.vk, "modifiers": k.mods}
		if k.text != "" {
			down["type"] = "keyDown"
			down["text"] = k.text
			down["unmodifiedText"] = k.text
		}
		up := map[string]any{"type": "keyUp", "key": k.key, "code": k.code, "windowsVirtualKeyCode": k.vk, "modifiers": k.mods}
		for _, p := range []map[string]any{down, up} {
			if err := c.call(page, "Input.dispatchKeyEvent", p, &res, ""); err != nil {
				t.Fatal(err)
			}
		}
	}
	tab := key{key: "Tab", code: "Tab", vk: 9}
	shiftTab := key{key: "Tab", code: "Tab", vk: 9, mods: 8}
	escape := key{key: "Escape", code: "Escape", vk: 27}
	eval := func(js string) string { t.Helper(); return c.eval(t, page, js) }
	waitEval := func(js, want string) {
		t.Helper()
		got := ""
		until(t, func() bool { got = eval(js); return got == want }, func() string { return js + ": " + got + ", want " + want })
	}
	gone := func(text string) {
		t.Helper()
		waitEval(`String(!__view(`+strconv.Quote(text)+`))`, "true")
	}

	// Open: a popover in the top layer over the root, its content under the anchor.
	tapOn("Choose")
	_, cy, _, ch := rect("Choose")
	_, ay, _, _ := rect("Apple")
	if ay < cy+ch+4-1 {
		t.Fatalf("Apple at %v, Choose at %v + %v", ay, cy, ch)
	}
	const layer = `document.querySelector(".asuka-overlay")`
	if got := eval(`(() => { const o = ` + layer + `, r = o.getBoundingClientRect(), a = document.querySelector(".asuka-root").getBoundingClientRect();
		return [o.matches(":popover-open"), getComputedStyle(o).position, r.left === a.left && r.top === a.top && r.width === a.width && r.height === a.height, getComputedStyle(o).pointerEvents].join(" "); })()`); got != "true fixed true none" {
		t.Fatalf("the overlay's layer: %q", got)
	}

	// The anchor scrolled: its content follows.
	const scroller = `document.querySelector(".asuka-scroller")`
	eval(scroller + `.scrollTop = 20, "ok"`)
	until(t, func() bool { _, y, _, _ := rect("Apple"); return y == ay-20 }, func() string { _, y, _, _ := rect("Apple"); return fmt.Sprintf("Apple at %v, want %v", y, ay-20) })
	// Scrolled out of view: it closes.
	eval(scroller + `.scrollTop = 300, "ok"`)
	showsLine(t, s, "closed: anchorHidden")
	gone("Apple")
	eval(scroller + `.scrollTop = 0, "ok"`)

	// A press outside: it closes, and the press reaches what it's on.
	tapOn("Choose")
	rect("Apple")
	tapOn("Elsewhere")
	showsLine(t, s, "closed: outsidePress")
	showsLine(t, s, "picked: elsewhere")
	gone("Apple")
	// Its own views take presses.
	tapOn("Choose")
	tapOn("Pear")
	showsLine(t, s, "picked: pear")
	gone("Pear")
	// Escape: the root's keys.
	tapOn("Choose")
	rect("Apple")
	press(escape)
	showsLine(t, s, "closed: escape")
	gone("Apple")

	// Modal: what's under it is inert; a press on it reaches the layer.
	tapOn("Show dialog")
	rect("Are you sure?")
	waitEval(`String(document.querySelector(".asuka-root").inert)`, "true")
	if got := eval(`(() => { return String(!!__view("Are you sure?").closest("[aria-modal=true]")); })()`); got != "true" {
		t.Fatalf("the dialog's aria-modal: %q", got)
	}
	bx, by, bw, bh := rect("Behind")
	click(bx+bw/2, by+bh/2)
	showsLine(t, s, "modal closed: outsidePress")
	showsLine(t, s, "behind: 0")
	gone("Are you sure?")
	waitEval(`String(document.querySelector(".asuka-root").inert)`, "false")
	tapOn("Behind")
	showsLine(t, s, "behind: 1")
	// Presented again, an overlay is over the others (the modal one's layer, empty, stays).
	tapOn("Choose")
	tapOn("Apple")
	showsLine(t, s, "picked: apple")
	gone("Apple")

	// Over a sheet: in the sheet's modal dialog, under its anchor there.
	tapOn("Open sheet")
	waitEval(`String(!!document.querySelector("dialog[open]") && document.querySelector("dialog[open]").getAnimations().length === 0)`, "true") // slid up
	tapOn("In sheet")
	_, iy, _, ih := rect("In sheet")
	_, py, _, _ := rect("Plum")
	if py < iy+ih-1 {
		t.Fatalf("Plum at %v, its anchor at %v + %v", py, iy, ih)
	}
	if got := eval(`(() => { const o = __view("Plum").closest(".asuka-overlay"); return o.parentElement.tagName + " " + o.matches(":popover-open"); })()`); got != "DIALOG true" {
		t.Fatalf("the sheet's overlay: %q", got)
	}
	tapOn("Plum")
	gone("Plum")
	tapOn("Close sheet")
	showsLine(t, s, "picked: plum")

	// A press focuses a focusable view, not visibly, and says where it began.
	tx, ty, _, _ := rect("Typeahead")
	click(tx+5, ty+3)
	showsLine(t, s, "A focused")
	showsLine(t, s, "focus: true")
	if at := lineOf(s, "at: "); at != "at: 5,3" {
		t.Fatalf("pressStart: %q", at)
	}
	// Typed: its keyText.
	press(key{key: "b", code: "KeyB", vk: 66, text: "b"})
	press(key{key: "a", code: "KeyA", vk: 65, text: "a"})
	showsLine(t, s, "typed: ba")
	// Shift+Tab: the focus leaves it; Tab: back, visibly.
	press(shiftTab)
	showsLine(t, s, "A idle")
	press(tab)
	showsLine(t, s, "A visible")
	showsLine(t, s, "focus: true false true")
	// NoFocusRing: no outline of the host's.
	if got := eval(`(() => { const a = document.activeElement; return [a.classList.contains("asuka-no-ring"), a.matches(":focus-visible"), getComputedStyle(a).outlineStyle].join(" "); })()`); got != "true true none" {
		t.Fatalf("hideFocusRing: %q", got)
	}

	// A focus trap: Tab goes round its views.
	tapOn("One")
	focused := `document.activeElement.textContent.trim()`
	waitEval(focused, "One")
	press(tab)
	waitEval(focused, "Two")
	press(tab)
	waitEval(focused, "One")
	press(shiftTab)
	waitEval(focused, "Two")

	// Screen readers' states.
	const box = `__view("Box").closest("[role=checkbox]")`
	if got := eval(`(() => { const b = ` + box + `, l = document.getElementById(b.getAttribute("aria-labelledby"));
		return ["role", "aria-checked", "aria-expanded", "aria-valuetext", "aria-valuemin", "aria-valuemax", "aria-valuenow"].map((a) => b.getAttribute(a)).join(" ") + " " + (l && l.textContent); })()`); got != "checkbox false false 40% 0 100 40 Agree" {
		t.Fatalf("the checkbox's ARIA: %q", got)
	}
	tapOn("Box")
	waitEval(box+`.getAttribute("aria-checked")`, "true")
	if t.Failed() {
		t.Log(strings.Join(s.texts(), "\n"))
	}
}
