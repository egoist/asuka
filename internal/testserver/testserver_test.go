package testserver

import (
	"bufio"
	"bytes"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// writeClientFrame writes a masked frame, as a client must.
func writeClientFrame(w io.Writer, op byte, payload []byte) {
	mask := [4]byte{1, 2, 3, 4}
	h := []byte{0x80 | op}
	if len(payload) < 126 {
		h = append(h, 0x80|byte(len(payload)))
	} else {
		h = append(h, 0x80|126, byte(len(payload)>>8), byte(len(payload)))
	}
	h = append(h, mask[:]...)
	b := append([]byte{}, payload...)
	for i := range b {
		b[i] ^= mask[i%4]
	}
	w.Write(append(h, b...))
}

func readServerFrame(t *testing.T, r *bufio.Reader) (byte, []byte) {
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
	io.ReadFull(r, p)
	return h[0] & 0x0f, p
}

// The WebSocket echo answers as a client of any platform expects.
func TestWebSocketEcho(t *testing.T) {
	s := httptest.NewServer(Handler())
	defer s.Close()
	c, err := net.Dial("tcp", strings.TrimPrefix(s.URL, "http://"))
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	io.WriteString(c, "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: other, chat\r\nX-Token: t1\r\n\r\n")
	r := bufio.NewReader(c)
	resp, err := http.ReadResponse(r, nil)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != 101 || resp.Header.Get("Sec-WebSocket-Accept") != "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" || resp.Header.Get("Sec-WebSocket-Protocol") != "chat" {
		t.Fatalf("handshake: %d %v", resp.StatusCode, resp.Header)
	}
	if op, p := readServerFrame(t, r); op != opText || string(p) != "header:t1" {
		t.Fatalf("first: %d %q", op, p)
	}
	writeClientFrame(c, opText, []byte("hi"))
	if op, p := readServerFrame(t, r); op != opText || string(p) != "hi" {
		t.Fatalf("echo: %d %q", op, p)
	}
	big := []byte(strings.Repeat("b", 300))
	writeClientFrame(c, opBinary, big)
	if op, p := readServerFrame(t, r); op != opBinary || string(p) != string(big) {
		t.Fatalf("binary echo: %d %d bytes", op, len(p))
	}
	writeClientFrame(c, opPing, []byte("p"))
	if op, p := readServerFrame(t, r); op != opPong || string(p) != "p" {
		t.Fatalf("pong: %d %q", op, p)
	}
	writeClientFrame(c, opText, []byte("close please"))
	op, p := readServerFrame(t, r)
	if op != opClose || binary.BigEndian.Uint16(p) != 4000 || string(p[2:]) != "bye" {
		t.Fatalf("close: %d %v", op, p)
	}
}

// The PKI's certificates as a Go client uses them: the server trusted
// with CA alone, /mtls answering a client certificate (EC, RSA) and
// refusing none; OtherCA trusting nothing.
func TestPKI(t *testing.T) {
	p, err := NewPKI()
	if err != nil {
		t.Fatal(err)
	}
	s := StartTLS(Handler(), p)
	defer s.Close()
	get := func(ca, cert, key string) (string, error) {
		pool := x509.NewCertPool()
		pool.AppendCertsFromPEM([]byte(ca))
		cfg := &tls.Config{RootCAs: pool}
		if cert != "" {
			c, err := tls.X509KeyPair([]byte(cert), []byte(key))
			if err != nil {
				t.Fatal(err)
			}
			cfg.Certificates = []tls.Certificate{c}
		}
		c := &http.Client{Transport: &http.Transport{TLSClientConfig: cfg}}
		r, err := c.Get(s.URL + "/mtls")
		if err != nil {
			return "", err
		}
		defer r.Body.Close()
		b, _ := io.ReadAll(r.Body)
		return fmt.Sprint(r.StatusCode, " ", strings.TrimSpace(string(b))), nil
	}
	for _, c := range []struct{ ca, cert, key, want string }{
		{p.CA, "", "", "403 no client certificate"},
		{p.CA, p.ClientCert, p.ClientKey, "200 client: Asuka Test Client"},
		{p.CA, p.ClientCert, p.ClientKeySEC1, "200 client: Asuka Test Client"},
		{p.CA, p.RSAClientCert, p.RSAClientKey, "200 client: Asuka Test RSA Client"},
	} {
		got, err := get(c.ca, c.cert, c.key)
		if err != nil || got != c.want {
			t.Errorf("got %q, %v; want %q", got, err, c.want)
		}
	}
	if _, err := get(p.OtherCA, "", ""); err == nil {
		t.Error("trusted with OtherCA")
	}
}

// /upload reads a body as it comes, chunked or not, and says its size and
// digest; ?answer answers before reading; /multipart parses a form's
// fields and files.
func TestUploads(t *testing.T) {
	s := httptest.NewServer(Handler())
	defer s.Close()
	get := func(r *http.Response, err error) map[string]any {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
		defer r.Body.Close()
		var v map[string]any
		if err := json.NewDecoder(r.Body).Decode(&v); err != nil {
			t.Fatal(err)
		}
		return v
	}
	body := strings.Repeat("a", 200000)
	sum := sha256.Sum256([]byte(body))
	v := get(http.Post(s.URL+"/upload", "video/mp4", strings.NewReader(body)))
	if v["bytes"] != 200000.0 || v["sha256"] != hex.EncodeToString(sum[:]) || v["type"] != "video/mp4" || v["length"] != 200000.0 || v["chunked"] != false {
		t.Errorf("upload: %v", v)
	}
	// A reader of unknown length goes chunked.
	v = get(http.Post(s.URL+"/upload", "", io.MultiReader(strings.NewReader(body))))
	if v["bytes"] != 200000.0 || v["length"] != -1.0 || v["chunked"] != true {
		t.Errorf("chunked: %v", v)
	}
	r, err := http.Post(s.URL+"/upload?answer=413", "", strings.NewReader("x"))
	if err != nil || r.StatusCode != 413 {
		t.Errorf("answer: %v %v", r, err)
	}
	var form bytes.Buffer
	mw := multipart.NewWriter(&form)
	mw.WriteField("title", "Holiday")
	fw, _ := mw.CreateFormFile("video", "a.mp4")
	io.WriteString(fw, body)
	mw.Close()
	v = get(http.Post(s.URL+"/multipart", mw.FormDataContentType(), &form))
	files, _ := v["files"].([]any)
	if v["fields"].(map[string]any)["title"] != "Holiday" || len(files) != 1 {
		t.Fatalf("multipart: %v", v)
	}
	f := files[0].(map[string]any)
	if f["name"] != "video" || f["filename"] != "a.mp4" || f["bytes"] != 200000.0 || f["sha256"] != hex.EncodeToString(sum[:]) || f["type"] != "application/octet-stream" {
		t.Errorf("multipart file: %v", f)
	}
	if r, _ := http.Post(s.URL+"/multipart", "text/plain", strings.NewReader("x")); r.StatusCode != 400 {
		t.Errorf("not a form: %d", r.StatusCode)
	}
}
