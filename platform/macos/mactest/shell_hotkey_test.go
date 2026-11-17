//go:build darwin

package mactest

import (
	"fmt"
	"testing"
)

// shellHotkeys are the hot keys registered, by the package's ids.
func shellHotkeys(t *testing.T, tr *Tree) []int {
	var ids []int
	l, _ := shellJSON(t, tr)["hotkeys"].([]any)
	for _, x := range l {
		ids = append(ids, int(x.(float64)))
	}
	return ids
}

// A hot key registers with Carbon; its event (sent through the
// application's event target, as the system sends it) answers "next";
// unregistered, it doesn't, nor does an id that isn't registered. "stop"
// fails the waiting poll. Keys a Mac hasn't, and the system's own
// shortcuts (⌘Tab), fail.
func TestShellHotkeyModule(t *testing.T) {
	a := Run(t, sysCallSrc+`
		fn reg(id: int, mods: int, key: string) -> bytes = host.u32(id).concat(host.u8(mods)).concat(host.str(key)).concat("a test's".bytes())
		state step = 0
		app {
		  Column {
		    Button("Next") { on press { step += 1 } }
		    // F19 with every modifier: no app or system has it.
		    Sys("hotkey", "register", reg(7001, 15, "f19")) { label: "register" }
		    if step >= 1 { Sys("hotkey", "next", bytes()) { label: "next" } }
		    if step >= 2 { Sys("hotkey", "unregister", host.u32(7001)) { label: "unregister" } }
		    if step >= 3 { Sys("hotkey", "next", bytes()) { label: "next2" } }
		    if step >= 4 { Sys("hotkey", "stop", bytes()) { label: "stop" } }
		    if step >= 5 {
		      Sys("hotkey", "register", reg(7002, 1, "nope")) { label: "nokey" }
		      Sys("hotkey", "register", reg(7003, 1, "tab")) { label: "cmdtab" }
		      Sys("hotkey", "register", reg(7004, 1, "k")) { label: "cmdk" }
		    }
		  }
		}
	`)
	sysPrefix(t, a, "register", "ok")
	a.Until(func(tr *Tree) bool { return fmt.Sprint(shellHotkeys(t, tr)) == "[7001]" })
	a.Do("tap Next")
	if s := sysTextNow(a, "next"); s != "pending" {
		t.Fatalf("next answered before a press: %s", s)
	}
	// Another id (another app's, or none registered) is dropped.
	a.Do("hotkey 9999", "hotkey 7001")
	sysWant(t, a, "next", "ok 591b0000")
	a.Do("tap Next")
	sysPrefix(t, a, "unregister", "ok")
	a.Until(func(tr *Tree) bool { return len(shellHotkeys(t, tr)) == 0 })
	a.Do("tap Next", "hotkey 7001")
	a.Do("tap Next")
	sysWant(t, a, "next2", "err stopped")
	a.Do("tap Next")
	sysWant(t, a, "nokey", "err no key")
	sysWant(t, a, "cmdtab", "err taken")
	sysPrefix(t, a, "cmdk", "ok")
}

// sysTextNow is what the Text labeled label shows now ("" if none).
func sysTextNow(a *App, label string) string {
	for _, n := range a.Tree().Kind("Text") {
		if len(n.Text) > len(label)+2 && n.Text[:len(label)+2] == label+": " {
			return n.Text[len(label)+2:]
		}
	}
	return ""
}

// Through the package: Hotkey in a component; a press runs its on press;
// gone, it lets the keys go.
func TestShellHotkeyPackage(t *testing.T) {
	a := Run(t, `
		import platform/hotkey
		state presses = 0
		state on = true
		app {
		  Column {
		    Text("presses {presses}")
		    Button("Off") { on press { on = false } }
		    if on {
		      hotkey.Hotkey(Shortcut(key: "f18", primary: true, shift: true, alt: true, control: true)) {
		        on press { presses += 1 }
		      }
		    }
		  }
		}
	`)
	tr := a.Until(func(tr *Tree) bool { return len(shellHotkeys(t, tr)) == 1 })
	id := shellHotkeys(t, tr)[0]
	a.Do(fmt.Sprintf("hotkey %d", id))
	a.UntilText("presses 1")
	a.Do(fmt.Sprintf("hotkey %d", id))
	a.UntilText("presses 2")
	a.Do("tap Off")
	a.Until(func(tr *Tree) bool { return len(shellHotkeys(t, tr)) == 0 })
}
