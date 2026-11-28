// Package testserver is the server std/http's and std/websocket's tests
// talk to on every platform (Swift on this Mac, Kotlin, the web's
// JavaScript under Node, the desktops' C): bodies that come over time,
// server-sent events, and a WebSocket echo (RFC 6455, its server side
// here: Go's library has none). Go tests use Handler with httptest; the
// others run cmd (go run ./internal/testserver/cmd), which prints its
// URL and serves until its input closes.
//
//	GET  /stream      "one\n", "two\n", "three\n", 50 ms apart (chunked)
//	GET  /sse         three events (event:, id:, multi-line data), 50 ms apart
//	GET  /stall       its head, then nothing: a client's timeout
//	GET  /status/404  404, body "missing"
//	ANY  /echo        200, JSON {"method", "uri", "body", "headers"} of the request
//	GET  /ws          a WebSocket that echoes text and binary messages;
//	                  it sends "header:<x-token>" first when the request
//	                  had X-Token, chooses "chat" from the protocols
//	                  asked for, closes with 4000 "bye" on the text
//	                  "close please", with 1000 on "close normally"
//	ANY  /upload      200, JSON {"bytes", "sha256", "type", "length",
//	                  "chunked", "method"} of the request's body, read as it
//	                  comes (never held): length is its Content-Length (-1
//	                  for none), chunked whether it came chunked; ?delay=ms
//	                  waits that long after each 64 KiB read (a slow
//	                  network, for progress), ?answer=413 answers that at
//	                  once, reading nothing
//	ANY  /multipart   200, JSON {"fields": {name: value}, "files": [{"name",
//	                  "filename", "type", "bytes", "sha256"}], "type",
//	                  "length", "chunked"} of a multipart/form-data body,
//	                  its parts read as they come; 400 with why for one
//	                  that isn't
//	GET  /mtls        over TLS (StartTLS, a PKI's certificate): "client:
//	                  <its name>" for the client's certificate, 403
//	                  without one
//
// Over HTTPS (StartTLS), the same paths with a test authority's
// certificate (NewPKI); cmd serves both.
package testserver

import (
	"bufio"
	"crypto/sha1"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"
)

// Handler serves the paths above.
func Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/stream", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		for _, s := range []string{"one\n", "two\n", "three\n"} {
			io.WriteString(w, s)
			w.(http.Flusher).Flush()
			time.Sleep(50 * time.Millisecond)
		}
	})
	mux.HandleFunc("/sse", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		w.Header().Set("Cache-Control", "no-cache")
		for _, s := range []string{
			": hello\n\ndata: first\n\n",
			"event: price\nid: 2\ndata: 1\ndata: 2\n\n",
			"retry: 100\ndata: last\n\n",
		} {
			io.WriteString(w, s)
			w.(http.Flusher).Flush()
			time.Sleep(50 * time.Millisecond)
		}
	})
	mux.HandleFunc("/stall", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain")
		w.WriteHeader(200)
		w.(http.Flusher).Flush()
		select {
		case <-r.Context().Done():
		case <-time.After(30 * time.Second):
		}
	})
	mux.HandleFunc("/status/404", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(404)
		io.WriteString(w, "missing")
	})
	mux.HandleFunc("/echo", func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		h := map[string]string{}
		for k := range r.Header {
			h[strings.ToLower(k)] = r.Header.Get(k)
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{"method": r.Method, "uri": r.RequestURI, "body": string(body), "headers": h})
	})
	mux.HandleFunc("/upload", upload)
	mux.HandleFunc("/multipart", multipartForm)
	mux.HandleFunc("/ws", serveWebSocket)
	mux.HandleFunc("/mtls", mtls)
	return mux
}

// ---- Uploads ----

// digest reads r to its end a piece at a time (after each 64 KiB read,
// delay), giving its size and SHA-256.
func digest(r io.Reader, delay time.Duration) (int64, string, error) {
	h := sha256.New()
	buf := make([]byte, 64*1024)
	var n int64
	for {
		k, err := io.ReadFull(r, buf)
		h.Write(buf[:k])
		n += int64(k)
		if err == io.EOF || err == io.ErrUnexpectedEOF {
			return n, hex.EncodeToString(h.Sum(nil)), nil
		}
		if err != nil {
			return n, "", err
		}
		if delay > 0 {
			time.Sleep(delay)
		}
	}
}

func chunked(r *http.Request) bool {
	for _, te := range r.TransferEncoding {
		if te == "chunked" {
			return true
		}
	}
	return false
}

func upload(w http.ResponseWriter, r *http.Request) {
	if a := r.URL.Query().Get("answer"); a != "" {
		code, _ := strconv.Atoi(a)
		w.Header().Set("Connection", "close")
		w.WriteHeader(code)
		io.WriteString(w, "refused")
		return
	}
	ms, _ := strconv.Atoi(r.URL.Query().Get("delay"))
	n, sum, err := digest(r.Body, time.Duration(ms)*time.Millisecond)
	if err != nil {
		http.Error(w, err.Error(), 400)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]any{"method": r.Method, "bytes": n, "sha256": sum, "type": r.Header.Get("Content-Type"),
		"length": r.ContentLength, "chunked": chunked(r)})
}

func multipartForm(w http.ResponseWriter, r *http.Request) {
	mr, err := r.MultipartReader()
	if err != nil {
		http.Error(w, err.Error(), 400)
		return
	}
	fields := map[string]string{}
	files := []map[string]any{}
	for {
		p, err := mr.NextPart()
		if err == io.EOF {
			break
		}
		if err != nil {
			http.Error(w, err.Error(), 400)
			return
		}
		if p.FileName() == "" {
			b, _ := io.ReadAll(io.LimitReader(p, 1<<20))
			fields[p.FormName()] = string(b)
			continue
		}
		n, sum, err := digest(p, 0)
		if err != nil {
			http.Error(w, err.Error(), 400)
			return
		}
		files = append(files, map[string]any{"name": p.FormName(), "filename": p.FileName(), "type": p.Header.Get("Content-Type"), "bytes": n, "sha256": sum})
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]any{"fields": fields, "files": files, "type": r.Header.Get("Content-Type"), "length": r.ContentLength, "chunked": chunked(r)})
}

// ---- WebSocket (RFC 6455), the server's side ----

const wsGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

const (
	opCont   = 0x0
	opText   = 0x1
	opBinary = 0x2
	opClose  = 0x8
	opPing   = 0x9
	opPong   = 0xa
)

func serveWebSocket(w http.ResponseWriter, r *http.Request) {
	if !strings.EqualFold(r.Header.Get("Upgrade"), "websocket") || r.Header.Get("Sec-WebSocket-Key") == "" {
		http.Error(w, "not a WebSocket request", http.StatusBadRequest)
		return
	}
	sum := sha1.Sum([]byte(r.Header.Get("Sec-WebSocket-Key") + wsGUID))
	protocol := ""
	for _, p := range strings.Split(r.Header.Get("Sec-WebSocket-Protocol"), ",") {
		if strings.TrimSpace(p) == "chat" {
			protocol = "chat"
		}
	}
	conn, rw, err := http.NewResponseController(w).Hijack()
	if err != nil {
		return
	}
	defer conn.Close()
	fmt.Fprintf(rw, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n", base64.StdEncoding.EncodeToString(sum[:]))
	if protocol != "" {
		fmt.Fprintf(rw, "Sec-WebSocket-Protocol: %s\r\n", protocol)
	}
	fmt.Fprint(rw, "\r\n")
	rw.Flush()
	if tok := r.Header.Get("X-Token"); tok != "" {
		writeFrame(rw.Writer, opText, []byte("header:"+tok))
		rw.Flush()
	}
	for {
		op, msg, err := readMessage(rw.Reader, rw.Writer)
		if err != nil {
			return
		}
		switch {
		case op == opClose:
			// Answer the client's close with its code, and end.
			writeFrame(rw.Writer, opClose, msg)
			rw.Flush()
			return
		case op == opText && string(msg) == "close please":
			writeFrame(rw.Writer, opClose, closePayload(4000, "bye"))
			rw.Flush()
			readMessage(rw.Reader, rw.Writer) // the client's answer
			return
		case op == opText && string(msg) == "close normally":
			writeFrame(rw.Writer, opClose, closePayload(1000, ""))
			rw.Flush()
			readMessage(rw.Reader, rw.Writer)
			return
		default:
			writeFrame(rw.Writer, op, msg)
			rw.Flush()
		}
	}
}

func closePayload(code int, reason string) []byte {
	b := make([]byte, 2, 2+len(reason))
	binary.BigEndian.PutUint16(b, uint16(code))
	return append(b, reason...)
}

// readMessage reads a whole message (its fragments joined), answering
// pings; a close is a message of its own.
func readMessage(r *bufio.Reader, w *bufio.Writer) (byte, []byte, error) {
	var op byte
	var msg []byte
	for {
		fin, o, payload, err := readFrame(r)
		if err != nil {
			return 0, nil, err
		}
		switch o {
		case opPing:
			writeFrame(w, opPong, payload)
			w.Flush()
			continue
		case opPong:
			continue
		case opClose:
			return opClose, payload, nil
		case opCont:
			if op == 0 {
				return 0, nil, errors.New("a continuation without a message")
			}
		default:
			op = o
		}
		msg = append(msg, payload...)
		if fin {
			return op, msg, nil
		}
	}
}

func readFrame(r *bufio.Reader) (fin bool, op byte, payload []byte, err error) {
	var h [2]byte
	if _, err = io.ReadFull(r, h[:]); err != nil {
		return
	}
	fin, op = h[0]&0x80 != 0, h[0]&0x0f
	masked := h[1]&0x80 != 0
	n := uint64(h[1] & 0x7f)
	switch n {
	case 126:
		var b [2]byte
		if _, err = io.ReadFull(r, b[:]); err != nil {
			return
		}
		n = uint64(binary.BigEndian.Uint16(b[:]))
	case 127:
		var b [8]byte
		if _, err = io.ReadFull(r, b[:]); err != nil {
			return
		}
		n = binary.BigEndian.Uint64(b[:])
	}
	if !masked {
		err = errors.New("a client's frame without a mask")
		return
	}
	var mask [4]byte
	if _, err = io.ReadFull(r, mask[:]); err != nil {
		return
	}
	if n > 16<<20 {
		err = errors.New("a frame too large")
		return
	}
	payload = make([]byte, n)
	if _, err = io.ReadFull(r, payload); err != nil {
		return
	}
	for i := range payload {
		payload[i] ^= mask[i%4]
	}
	return
}

func writeFrame(w *bufio.Writer, op byte, payload []byte) {
	w.WriteByte(0x80 | op)
	switch n := len(payload); {
	case n < 126:
		w.WriteByte(byte(n))
	case n < 1<<16:
		w.WriteByte(126)
		var b [2]byte
		binary.BigEndian.PutUint16(b[:], uint16(n))
		w.Write(b[:])
	default:
		w.WriteByte(127)
		var b [8]byte
		binary.BigEndian.PutUint64(b[:], uint64(n))
		w.Write(b[:])
	}
	w.Write(payload)
}

// Listener is a listener on the loopback interface at a port the system
// picks.
func Listener() (net.Listener, error) { return net.Listen("tcp", "127.0.0.1:0") }
