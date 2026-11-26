package windows_test

import (
	"bytes"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// hostAddress is the address the program under test reaches this
// computer by: the VM's gateway, or the loopback on Windows itself.
func hostAddress(t *testing.T, w *windows) string {
	if w.vm == "" {
		return "127.0.0.1"
	}
	out, err := exec.Command("swiftvm", "exec", w.vm, "--", "(Get-NetRoute -DestinationPrefix 0.0.0.0/0 | Select-Object -First 1).NextHop").Output()
	if err != nil {
		t.Skipf("the VM's gateway: %v", err)
	}
	return strings.TrimSpace(string(out))
}

// The host's modules (testdata/modules.asuka): platform/host's call, a
// module the host
// hasn't, platform/apps' ranking, a tray icon, and the updater: info, a
// download over HTTP with its progress, and an install that swaps the
// app's own folder while it runs.
func TestModules(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)

	// What the update downloads: 300 KB, its length known.
	body := bytes.Repeat([]byte("asuka "), 50000)
	ln, err := net.Listen("tcp", ":0")
	if err != nil {
		t.Fatal(err)
	}
	srv := &http.Server{Handler: http.HandlerFunc(func(rw http.ResponseWriter, r *http.Request) {
		if r.Header.Get("User-Agent") != "asuka-updater" {
			http.Error(rw, "User-Agent "+r.Header.Get("User-Agent"), http.StatusBadRequest)
			return
		}
		http.ServeContent(rw, r, "new.txt", time.Time{}, bytes.NewReader(body))
	})}
	go srv.Serve(ln)
	defer srv.Close()
	port := ln.Addr().(*net.TCPAddr).Port
	url := "http://" + hostAddress(t, w) + ":" + itoa(port) + "/new.txt"

	src, err := os.ReadFile(filepath.Join("testdata", "modules.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	exe := buildApp(t, w, strings.ReplaceAll(string(src), "MODULES_URL", url), local)

	// The installed version: its file list names app.exe and old.txt;
	// keep.txt is the user's. The update, staged: app.exe and new.txt
	// (downloaded).
	write := func(name, s string) {
		if err := os.WriteFile(filepath.Join(local, name), []byte(s), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write(".asuka-files", "app.exe\nold.txt\n")
	write("old.txt", "old")
	write("keep.txt", "keep")
	staged := filepath.Join(local, "staged-test")
	if err := os.MkdirAll(staged, 0o755); err != nil {
		t.Fatal(err)
	}
	b, err := os.ReadFile(exe)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(staged, "app.exe"), b, 0o755); err != nil {
		t.Fatal(err)
	}

	d := script(t, w, local, guest, "app.exe", "wait:6s", "snap:done")
	dump := d["done"]
	t.Logf("%s", dump)
	shows(t, dump, "the modules", []string{
		"supported: 1",
		"host: platform/push: Windows gives push channels (WNS) to packaged apps only",
		"missing: unsupported: nope isn't on Windows",
		"ranked: Visual Studio Code|Visual Studio Code,Xcode",
		"info: false notConfigured folder windows-" + w.arch,
		"download: 200 300000/300000 [] polls>1:true",
		"install: installed",
	})

	got, err := os.ReadFile(filepath.Join(local, "new.txt"))
	if err != nil || !bytes.Equal(got, body) {
		t.Errorf("new.txt isn't the download (%d bytes, %v)", len(got), err)
	}
	if _, err := os.Stat(filepath.Join(local, "old.txt")); !os.IsNotExist(err) {
		t.Errorf("old.txt, which the update no longer has, stayed: %v", err)
	}
	if _, err := os.Stat(filepath.Join(local, "keep.txt")); err != nil {
		t.Errorf("keep.txt, which no list names, went: %v", err)
	}
	if list, _ := os.ReadFile(filepath.Join(local, ".asuka-files")); string(list) != "app.exe\nnew.txt\n" {
		t.Errorf(".asuka-files = %q", list)
	}
	if _, err := os.Stat(staged); !os.IsNotExist(err) {
		t.Errorf("the staged update stayed: %v", err)
	}
	aside, _ := filepath.Glob(filepath.Join(local, ".app.exe.old-*"))
	if len(aside) != 1 {
		t.Errorf("the running app.exe put aside: %v", aside)
	}
}

// More of the modules (testdata/services.asuka), through platform/host and
// platform/host: a toast shown, listed by the notification center's
// history and removed by its thread; a global hot key; recent documents;
// start at login (the user's Run key, set and taken away); the taskbar's
// progress, badge and attention; what Windows hasn't.
func TestServices(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	if err := os.WriteFile(filepath.Join(local, "doc.txt"), []byte("doc"), 0o644); err != nil {
		t.Fatal(err)
	}
	src, err := os.ReadFile(filepath.Join("testdata", "services.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	file := strings.ReplaceAll(guest+`\doc.txt`, `\`, `\\`)
	buildApp(t, w, strings.ReplaceAll(string(src), "SERVICES_FILE", file), local)
	d := script(t, w, local, guest, "app.exe", "wait:5s", "snap:done")
	dump := d["done"]
	t.Logf("%s", dump)
	shows(t, dump, "the services", []string{
		"toast: enabled:1 shown:1 Hello after:0",
		"hotkey: registered",
		"recent: true []",
		"login: 100",
		"dock: attention 4",
		"contacts: 1 not allowed",
		"widget: not available: widgets are iOS's and Android's",
	})
}

func itoa(i int) string {
	var b [20]byte
	n := len(b)
	for {
		n--
		b[n] = byte('0' + i%10)
		i /= 10
		if i == 0 {
			break
		}
	}
	return string(b[n:])
}

// platform/transcribe (testdata/transcribe.asuka): SAPI's dictation, on
// the device, hears a spoken sentence in an .m4a and a WAV (made by
// macOS's say), with its words' timing; listening ends with finish.
func TestTranscribe(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	for _, f := range []string{"speech.m4a", "speech.wav"} {
		b, err := os.ReadFile(filepath.Join("testdata", f))
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(local, f), b, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	src, err := os.ReadFile(filepath.Join("testdata", "transcribe.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	dir := strings.ReplaceAll(guest, `\`, `\\`)
	buildStdApp(t, w, strings.ReplaceAll(string(src), "TRANSCRIBE_DIR", dir), local, "transcribe")
	d := script(t, w, local, guest, "app.exe", "waitfor:"+guest+`\transcribed`, "wait:1s", "tap:Finish", "waitfor:"+guest+`\listened`, "snap:done")
	dump := d["done"]
	t.Logf("%s", dump)
	shows(t, dump, "transcribed", []string{
		"check: true en-US",
		"absent: false tlh",
		"missing: can't read",
		"other: TranscribeError.notAvailable",
	})
	// Listening ends as Finish is pressed: with what was heard (the VM has
	// no microphone, so nothing), or at once with notAvailable.
	if !strings.Contains(dump, `"listened: final '' after 1"`) && !strings.Contains(dump, `"listened: TranscribeError.notAvailable"`) {
		t.Errorf("listening didn't end as it should:\n%s", dump)
	}
	// The desktop recognizer's dictation isn't word for word, but hears
	// most of the sentence, in both.
	for _, f := range []string{"m4a: ", "wav: "} {
		i := strings.Index(dump, f)
		if i < 0 {
			t.Fatalf("no %q in the dump", f)
		}
		line := dump[i:]
		if j := strings.IndexByte(line, '\n'); j >= 0 {
			line = line[:j]
		}
		heard := 0
		for _, word := range []string{"quick", "brown", "fox", "jumps", "over", "lazy", "dog"} {
			if strings.Contains(line, word) {
				heard++
			}
		}
		if heard < 5 || !strings.Contains(line, "| en-US | timed:true") {
			t.Errorf("%s: heard %d of 7 words, or no language or timing", line, heard)
		}
	}
}
