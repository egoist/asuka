//go:build e2e

package e2e

import (
	"bufio"
	"fmt"
	"net"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// TestCallWeb: examples/call in two headless Chrome pages, each its own
// dev server, joined to one room of the example's signaling server: they
// connect, each decodes the other's video (Chrome's fake camera on one,
// the app's test pattern on the other, as stats report its size), and a
// message on the data channel comes back echoed.
func TestCallWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	signal := callServer(t)
	page := func(query string) *webScreen {
		url := webServer(t, "call")
		p := exec.Command(browser, "--headless=new", "--no-first-run", "--no-default-browser-check", "--disable-gpu",
			"--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream", "--autoplay-policy=no-user-gesture-required",
			"--user-data-dir="+t.TempDir(), "--window-size=430,932", url)
		if err := p.Start(); err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { p.Process.Kill(); p.Wait() })
		s := &webScreen{t: t, url: url}
		waitFor(t, s, []string{"Join"})
		s.command("visit /rooms/e2e?server=" + signal + query)
		time.Sleep(time.Second) // the page goes; the next command waits for the new one
		return s
	}
	caller := page("")
	callee := page("&pattern=true")
	for _, s := range []*webScreen{caller, callee} {
		until(t, func() bool {
			for _, l := range s.texts() {
				if strings.HasPrefix(l, "connected · ICE c") && strings.Contains(strings.Join(s.texts(), "\n"), "×") {
					return true
				}
			}
			return false
		}, func() string { return fmt.Sprintf("connected, with video; the page shows %q", s.texts()) })
	}
	caller.tap("Send")
	waitFor(t, caller, []string{"me: hello", "them: re: hello"})
	waitFor(t, callee, []string{"them: hello"})
	callee.tap("Hang up")
	waitFor(t, caller, []string{"The other side left"})
}

// callServer runs examples/call's signaling server on a port of its own:
// its ws:// URL.
func callServer(t *testing.T) string {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	addr := l.Addr().String()
	l.Close()
	bin := filepath.Join(t.TempDir(), "callserver")
	build := exec.Command("go", "build", "-o", bin, ".")
	build.Dir = filepath.Join(root(t), "examples", "call", "server")
	if b, err := build.CombinedOutput(); err != nil {
		t.Fatalf("building the signaling server: %v\n%s", err, b)
	}
	srv := exec.Command(bin, "-addr", addr)
	errs, _ := srv.StderrPipe()
	if err := srv.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { srv.Process.Kill(); srv.Wait() })
	bufio.NewReader(errs).ReadString('\n') // it says it listens
	for range 50 {
		if c, err := net.Dial("tcp", addr); err == nil {
			c.Close()
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	return "ws://" + addr
}
