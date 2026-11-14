package main

import (
	"os"
	"testing"

	"github.com/egoist/asuka/host/fake"
	"github.com/egoist/asuka/uitest"
)

func init() { today = func() string { return "2026-11-15" } }

// mount runs the journal with a fake iCloud, its containers in a test's
// directory.
func mount(t *testing.T, setup func(ic *fake.ICloud)) (*uitest.App, *fake.ICloud) {
	var ic *fake.ICloud
	a := uitest.Mount(t, Journal, uitest.Setup(func(h *fake.Host) {
		ic = h.ICloud()
		ic.Root = t.TempDir()
		if setup != nil {
			setup(ic)
		}
	}))
	return a, ic
}

func contents(t *testing.T, ic *fake.ICloud, rel string) string {
	t.Helper()
	b, err := os.ReadFile(ic.Path(rel))
	if err != nil {
		return "(" + err.Error() + ")"
	}
	return string(b)
}

// Today's entry is written to iCloud Drive and saved there; another
// device's comes listed before it's downloaded, and downloads as it
// opens.
func TestEntries(t *testing.T) {
	a, ic := mount(t, nil)
	a.Until("the documents", func() bool { return !a.Disabled("New Entry") })
	a.Tap("New Entry")
	a.WaitFor("2026-11-15")
	a.Type("What happened today?", "Walked to the lake.")
	a.Tap("Save")
	a.WaitFor("Saved")
	if got := contents(t, ic, "Documents/2026-11-15.txt"); got != "Walked to the lake." {
		t.Fatalf("saved %q", got)
	}

	ic.Arrive("Documents/2026-11-14.txt", []byte("Rain all day."))
	a.WaitFor("2026-11-14")
	a.Expect("In iCloud")
	a.Tap("2026-11-14")
	a.WaitFor("Rain all day.")
	if !ic.Downloaded("Documents/2026-11-14.txt") {
		t.Error("opened without downloading")
	}
	a.ExpectNo("In iCloud")
}

// The text size is in iCloud's key-value store: the app's change goes
// there, another device's comes here.
func TestTextSize(t *testing.T) {
	a, ic := mount(t, nil)
	a.WaitFor("Text size 17")
	a.Tap("A+")
	a.WaitFor("Text size 18")
	a.Until("the value kept", func() bool { v, _ := ic.Value("textSize"); return string(v) == "18" })
	ic.Change("textSize", 22)
	a.WaitFor("Text size 22")
}

// A size kept before, on any device, is the one the app starts with.
func TestTextSizeKept(t *testing.T) {
	a, _ := mount(t, func(ic *fake.ICloud) { ic.Change("textSize", 20) })
	a.WaitFor("Text size 20")
}

// An entry two devices changed shows the conflict; keeping theirs makes
// it the entry.
func TestConflict(t *testing.T) {
	a, ic := mount(t, func(ic *fake.ICloud) {
		os.MkdirAll(ic.Path("Documents"), 0o755)
		os.WriteFile(ic.Path("Documents/2026-11-13.txt"), []byte("Mine"), 0o644)
	})
	a.WaitFor("2026-11-13")
	a.Tap("2026-11-13")
	a.WaitFor("Mine")
	ic.Conflict("Documents/2026-11-13.txt", []byte("Theirs"), "Ann's iPad")
	a.WaitFor("Changed on another device too.")
	a.Tap("Keep Theirs")
	a.WaitFor("Theirs")
	a.ExpectNo("Changed on another device too.", "Conflict")
	if got := contents(t, ic, "Documents/2026-11-13.txt"); got != "Theirs" {
		t.Errorf("kept %q", got)
	}
}

// Signed out, the app says how to keep the journal, and can't write one.
func TestSignedOut(t *testing.T) {
	a, ic := mount(t, func(ic *fake.ICloud) { ic.SignOut() })
	a.WaitFor("Sign in to iCloud, with iCloud Drive on, to keep your journal on all your devices.")
	if !a.Disabled("New Entry") {
		t.Error("New Entry without an account")
	}
	ic.SignIn("test")
	a.Until("the documents", func() bool { return !a.Disabled("New Entry") })
	a.ExpectNo("Sign in to iCloud, with iCloud Drive on, to keep your journal on all your devices.")
}
