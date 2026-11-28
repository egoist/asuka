package main

import (
	"bufio"
	"encoding/binary"
	"io"
	"net"
	"net/http/httptest"
	"strings"
	"testing"
)

// dial opens a WebSocket to the relay's room name, as a client does.
func dial(t *testing.T, url, room string) (net.Conn, *bufio.Reader) {
	t.Helper()
	c, err := net.Dial("tcp", strings.TrimPrefix(url, "http://"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	io.WriteString(c, "GET /rooms/"+room+" HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n")
	r := bufio.NewReader(c)
	status, _ := r.ReadString('\n')
	if !strings.Contains(status, "101") {
		t.Fatalf("status %q", status)
	}
	for {
		l, err := r.ReadString('\n')
		if err != nil {
			t.Fatal(err)
		}
		if l == "\r\n" {
			break
		}
		if strings.HasPrefix(l, "Sec-WebSocket-Accept:") && strings.TrimSpace(strings.TrimPrefix(l, "Sec-WebSocket-Accept:")) != "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" {
			t.Fatalf("accept %q", l)
		}
	}
	return c, r
}

func sendText(t *testing.T, c net.Conn, s string) {
	t.Helper()
	mask := [4]byte{1, 2, 3, 4}
	b := []byte{0x80 | opText, 0x80 | byte(len(s))}
	b = append(b, mask[:]...)
	for i := range len(s) {
		b = append(b, s[i]^mask[i%4])
	}
	if _, err := c.Write(b); err != nil {
		t.Fatal(err)
	}
}

func readText(t *testing.T, r *bufio.Reader) string {
	t.Helper()
	var h [2]byte
	if _, err := io.ReadFull(r, h[:]); err != nil {
		t.Fatal(err)
	}
	n := int(h[1] & 0x7f)
	if n == 126 {
		var b [2]byte
		io.ReadFull(r, b[:])
		n = int(binary.BigEndian.Uint16(b[:]))
	}
	p := make([]byte, n)
	if _, err := io.ReadFull(r, p); err != nil {
		t.Fatal(err)
	}
	if h[0]&0x0f != opText {
		t.Fatalf("op %x", h[0]&0x0f)
	}
	return string(p)
}

func TestRoom(t *testing.T) {
	s := httptest.NewServer(newRelay())
	defer s.Close()

	a, ar := dial(t, s.URL, "r")
	b, br := dial(t, s.URL, "r")
	if got := readText(t, ar); got != `{"type":"joined"}` {
		t.Fatalf("a got %q", got)
	}
	offer := `{"type":"offer","sdp":"` + strings.Repeat("v", 300) + `"}`
	sendText(t, a, offer[:100]) // a short one, then a long one (a 16-bit length)
	if got := readText(t, br); got != offer[:100] {
		t.Fatalf("b got %q", got)
	}
	sendText(t, b, `{"type":"answer","sdp":"x"}`)
	if got := readText(t, ar); got != `{"type":"answer","sdp":"x"}` {
		t.Fatalf("a got %q", got)
	}

	// A third is turned away; another room is its own.
	c, err := net.Dial("tcp", strings.TrimPrefix(s.URL, "http://"))
	if err != nil {
		t.Fatal(err)
	}
	io.WriteString(c, "GET /rooms/r HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: a2V5\r\nSec-WebSocket-Version: 13\r\n\r\n")
	status, _ := bufio.NewReader(c).ReadString('\n')
	c.Close()
	if !strings.Contains(status, "409") {
		t.Fatalf("third: %q", status)
	}
	dial(t, s.URL, "other")

	b.Close()
	if got := readText(t, ar); got != `{"type":"left"}` {
		t.Fatalf("a got %q", got)
	}
}
