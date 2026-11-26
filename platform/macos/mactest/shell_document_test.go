//go:build darwin

package mactest

import (
	"os"
	"os/exec"
	"path/filepath"
	"syscall"
	"testing"
	"time"
)

// platform/document's versions and lock: Save keeps the file as it was,
// Browse Versions… shows it and Restore brings it back; Lock locks the
// file (the Finder's Locked flag in an app built with the package's Swift;
// the test's player has none, so it's made read-only), an edit then asks
// (Cancel reverts it) and Unlock unlocks it.
func TestShellDocumentVersionsAndLock(t *testing.T) {
	path := filepath.Join(t.TempDir(), "notes.txt")
	if err := os.WriteFile(path, []byte("first"), 0o644); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		exec.Command("chflags", "nouchg", path).Run()
		os.Chmod(path, 0o644)
	})
	a := Run(t, `
		import platform/document
		let types = [document.Type(name: "Plain Text", extensions: ["txt"])]
		app { document.Documents(types) { editor: doc => Editor(doc) } }
		component Editor(doc: document.Document<string>) {
		  Column {
		    padding: 12
		    gap: 8
		    Text("{doc.name}: {doc.content ?? ""}")
		    Text("{doc.name} locked {doc.locked}")
		    Button("Change {doc.name}") { on press { doc.edit("second") } }
		  }
		}
	`)
	a.Do("openurls " + path)
	a.UntilText("notes.txt: first")
	a.Do("tap Change notes.txt")
	a.Do("menu Save")
	until(t, func() bool { b, _ := os.ReadFile(path); return string(b) == "second" })

	a.Do("menu Browse Versions…")
	a.UntilText("Versions of “notes.txt”")
	a.Do("tap Restore")
	a.UntilText("notes.txt: first")
	until(t, func() bool { b, _ := os.ReadFile(path); return string(b) == "first" })

	a.Do("menu Lock")
	a.UntilText("notes.txt locked true")
	if !lockedOnDisk(path) {
		t.Fatal("Lock didn't lock the file")
	}
	a.Do("tap Change notes.txt")
	a.UntilText("The file “notes.txt” is locked.")
	a.Do("tap Cancel")
	a.UntilText("notes.txt: first")
	if b, _ := os.ReadFile(path); string(b) != "first" {
		t.Fatalf("a locked file was written: %q", b)
	}
	a.Do("menu Unlock")
	a.UntilText("notes.txt locked false")
	if lockedOnDisk(path) {
		t.Fatal("Unlock didn't unlock the file")
	}
}

// lockedOnDisk reports whether path is locked: the Finder's Locked flag
// (uchg), or no write permission.
func lockedOnDisk(path string) bool {
	var st syscall.Stat_t
	return syscall.Stat(path, &st) == nil && (st.Flags&0x2 != 0 || st.Mode&0o200 == 0) // UF_IMMUTABLE
}

// until waits for ok, a minute at most.
func until(t *testing.T, ok func() bool) {
	t.Helper()
	for i := 0; i < 600; i++ {
		if ok() {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatal("timed out")
}
