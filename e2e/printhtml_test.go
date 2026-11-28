//go:build e2e

package e2e

import (
	"encoding/json"
	"strings"
	"testing"
)

// TestPrintHTMLWeb runs e2e/testdata/print-html in headless Chrome:
// print.html prints the markup it's given, not the frame's empty first
// document, and returns. Each document's print is replaced, before any
// of its scripts run, by one that records the text of the page it was
// called on, so no dialog opens.
func TestPrintHTMLWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	pageURL := webServer(t, "e2e/testdata/print-html")
	c := launchCDP(t, browser)
	var created struct{ TargetID string }
	if err := c.call("", "Target.createTarget", map[string]any{"url": "about:blank"}, &created, ""); err != nil {
		t.Fatal(err)
	}
	page := c.attach(t, created.TargetID)
	var res json.RawMessage
	if err := c.call(page, "Page.enable", nil, &res, ""); err != nil {
		t.Fatal(err)
	}
	hook := `window.print = function () { (top.__printed = top.__printed || []).push(document.body ? document.body.innerText : "") }`
	if err := c.call(page, "Page.addScriptToEvaluateOnNewDocument", map[string]any{"source": hook}, &res, ""); err != nil {
		t.Fatal(err)
	}
	if err := c.call(page, "Page.navigate", map[string]any{"url": pageURL}, &res, ""); err != nil {
		t.Fatal(err)
	}
	s := &webScreen{t: t, url: pageURL}
	showsLine(t, s, "status: ready")

	s.tap("Print")
	printed := func() string { return c.eval(t, page, `JSON.stringify(top.__printed || [])`) }
	until(t, func() bool { return strings.Contains(printed(), "Page two") }, func() string {
		return "printed: " + printed() + "\n" + strings.Join(s.texts(), "\n")
	})
	var got []string
	if err := json.Unmarshal([]byte(printed()), &got); err != nil {
		t.Fatal(err)
	}
	if len(got) != 1 || !strings.Contains(got[0], "Page one") {
		t.Errorf("printed %q; want one print, of both pages", got)
	}
	showsLine(t, s, "status: done")
}
