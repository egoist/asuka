package main

import (
	"bufio"
	"crypto/rand"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"
)

// cdp is a connection to a target of a Chrome's DevTools (the Chrome
// DevTools Protocol over a WebSocket): what asuka test --gui asks of
// the headless Chrome its web flows run in (its screenshots, its
// permissions). Just enough of both.
type cdp struct {
	mu   sync.Mutex
	conn net.Conn
	r    *bufio.Reader
	id   int
}

// cdpTarget is the WebSocket of the first target of a Chrome's DevTools
// at addr (host:port) of a type ("page") whose URL starts with prefix,
// waited for (Chrome makes its page a moment after it listens).
func cdpTarget(addr, typ, prefix string) (string, error) {
	deadline := time.Now().Add(15 * time.Second)
	for time.Now().Before(deadline) {
		var targets []struct{ Type, URL, WebSocketDebuggerURL string }
		if resp, err := http.Get("http://" + addr + "/json/list"); err == nil {
			json.NewDecoder(resp.Body).Decode(&targets)
			resp.Body.Close()
		}
		for _, t := range targets {
			if t.Type == typ && strings.HasPrefix(t.URL, prefix) {
				return t.WebSocketDebuggerURL, nil
			}
		}
		time.Sleep(100 * time.Millisecond)
	}
	return "", fmt.Errorf("Chrome has no %s at %s", typ, prefix)
}

func dialCDP(raw string) (*cdp, error) {
	u, err := url.Parse(raw)
	if err != nil {
		return nil, err
	}
	conn, err := net.DialTimeout("tcp", u.Host, 10*time.Second)
	if err != nil {
		return nil, err
	}
	key := make([]byte, 16)
	rand.Read(key)
	fmt.Fprintf(conn, "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
		u.RequestURI(), u.Host, base64.StdEncoding.EncodeToString(key))
	r := bufio.NewReader(conn)
	resp, err := http.ReadResponse(r, nil)
	if err != nil {
		conn.Close()
		return nil, err
	}
	if resp.StatusCode != http.StatusSwitchingProtocols {
		conn.Close()
		return nil, fmt.Errorf("DevTools' WebSocket: %s", resp.Status)
	}
	return &cdp{conn: conn, r: r}, nil
}

func (d *cdp) close() { d.conn.Close() }

// send writes one masked text frame, as a client must.
func (d *cdp) send(msg []byte) error {
	h := []byte{0x81}
	switch n := len(msg); {
	case n < 126:
		h = append(h, 0x80|byte(n))
	case n < 1<<16:
		h = append(h, 0x80|126)
		h = binary.BigEndian.AppendUint16(h, uint16(n))
	default:
		h = append(h, 0x80|127)
		h = binary.BigEndian.AppendUint64(h, uint64(n))
	}
	mask := make([]byte, 4)
	rand.Read(mask)
	h = append(h, mask...)
	body := make([]byte, len(msg))
	for i, c := range msg {
		body[i] = c ^ mask[i%4]
	}
	_, err := d.conn.Write(append(h, body...))
	return err
}

// receive reads one message, joining its frames.
func (d *cdp) receive() ([]byte, error) {
	var msg []byte
	for {
		var h [2]byte
		if _, err := io.ReadFull(d.r, h[:]); err != nil {
			return nil, err
		}
		n := uint64(h[1] & 0x7f)
		switch n {
		case 126:
			var b [2]byte
			if _, err := io.ReadFull(d.r, b[:]); err != nil {
				return nil, err
			}
			n = uint64(binary.BigEndian.Uint16(b[:]))
		case 127:
			var b [8]byte
			if _, err := io.ReadFull(d.r, b[:]); err != nil {
				return nil, err
			}
			n = binary.BigEndian.Uint64(b[:])
		}
		body := make([]byte, n)
		if _, err := io.ReadFull(d.r, body); err != nil {
			return nil, err
		}
		switch op := h[0] & 0x0f; op {
		case 0x8:
			return nil, errors.New("DevTools' WebSocket closed")
		case 0x9, 0xa:
			continue
		}
		msg = append(msg, body...)
		if h[0]&0x80 != 0 {
			return msg, nil
		}
	}
}

// call sends a command and waits for its result (result nil: ignored);
// the events that come meanwhile are dropped.
func (d *cdp) call(method string, params any, result any) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.id++
	if params == nil {
		params = map[string]any{}
	}
	req, _ := json.Marshal(map[string]any{"id": d.id, "method": method, "params": params})
	if err := d.send(req); err != nil {
		return err
	}
	d.conn.SetReadDeadline(time.Now().Add(30 * time.Second))
	for {
		b, err := d.receive()
		if err != nil {
			return fmt.Errorf("%s: %v", method, err)
		}
		var resp struct {
			ID     int
			Result json.RawMessage
			Error  *struct{ Message string }
		}
		if json.Unmarshal(b, &resp) != nil || resp.ID != d.id {
			continue
		}
		if resp.Error != nil {
			return fmt.Errorf("%s: %s", method, resp.Error.Message)
		}
		if result == nil {
			return nil
		}
		return json.Unmarshal(resp.Result, result)
	}
}

// screenshot is the page's viewport as a PNG.
func (d *cdp) screenshot() ([]byte, error) {
	var r struct{ Data string }
	if err := d.call("Page.captureScreenshot", map[string]any{"format": "png"}, &r); err != nil {
		return nil, err
	}
	return base64.StdEncoding.DecodeString(r.Data)
}

// evaluate runs JavaScript in the page.
func (d *cdp) evaluate(js string) error {
	var r struct {
		ExceptionDetails *struct{ Text string }
	}
	if err := d.call("Runtime.evaluate", map[string]any{"expression": js, "awaitPromise": true}, &r); err != nil {
		return err
	}
	if r.ExceptionDetails != nil {
		return errors.New(r.ExceptionDetails.Text)
	}
	return nil
}
