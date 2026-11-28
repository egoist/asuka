// Command server is the call example's signaling server: rooms of two,
// over WebSockets (RFC 6455's server side, with Go's standard library
// only). A client joins a room at ws://host:8080/rooms/<name>; when the
// second one joins, the first is told {"type":"joined"} (it calls), and
// from then on every text message one sends goes to the other as it is:
// the offer, the answer, the candidates. When one leaves, the other is
// told {"type":"left"}. A third client is turned away (409).
//
//	go run .                  # :8080
//	go run . -addr :9000
package main

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"flag"
	"io"
	"log"
	"net"
	"net/http"
	"strings"
	"sync"
)

func main() {
	addr := flag.String("addr", ":8080", "where to listen")
	flag.Parse()
	log.Printf("signaling on %s (ws://localhost%s/rooms/<name>)", *addr, *addr)
	log.Fatal(http.ListenAndServe(*addr, newRelay()))
}

// relay is the rooms, by name.
type relay struct {
	mu    sync.Mutex
	rooms map[string][]*client
}

// client is a WebSocket connection in a room: what it's sent goes through
// out, written by its own goroutine, so a slow client holds no one up.
type client struct {
	out  chan []byte
	done chan struct{}
}

func newRelay() *relay { return &relay{rooms: map[string][]*client{}} }

func (rl *relay) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	name, ok := strings.CutPrefix(r.URL.Path, "/rooms/")
	if !ok || name == "" || strings.Contains(name, "/") {
		http.NotFound(w, r)
		return
	}
	if !strings.EqualFold(r.Header.Get("Upgrade"), "websocket") || r.Header.Get("Sec-WebSocket-Key") == "" {
		http.Error(w, "a WebSocket, please", http.StatusBadRequest)
		return
	}
	c := &client{out: make(chan []byte, 64), done: make(chan struct{})}
	rl.mu.Lock()
	others := rl.rooms[name]
	if len(others) >= 2 {
		rl.mu.Unlock()
		http.Error(w, "the room is full", http.StatusConflict)
		return
	}
	rl.rooms[name] = append(others, c)
	rl.mu.Unlock()
	defer rl.leave(name, c)

	conn, rw, err := http.NewResponseController(w).Hijack()
	if err != nil {
		return
	}
	defer conn.Close()
	sum := sha1.Sum([]byte(r.Header.Get("Sec-WebSocket-Key") + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
	io.WriteString(rw, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "+base64.StdEncoding.EncodeToString(sum[:])+"\r\n\r\n")
	if err := rw.Flush(); err != nil {
		return
	}
	go c.write(conn, rw.Writer)
	if len(others) == 1 {
		others[0].send([]byte(`{"type":"joined"}`))
	}
	for {
		op, msg, err := readMessage(rw.Reader, c)
		if err != nil || op == opClose {
			if op == opClose {
				c.control(opClose, msg)
			}
			return
		}
		if op != opText {
			continue
		}
		for _, o := range rl.peers(name, c) {
			o.send(msg)
		}
	}
}

// peers are the others in room name.
func (rl *relay) peers(name string, c *client) []*client {
	rl.mu.Lock()
	defer rl.mu.Unlock()
	var out []*client
	for _, o := range rl.rooms[name] {
		if o != c {
			out = append(out, o)
		}
	}
	return out
}

func (rl *relay) leave(name string, c *client) {
	rl.mu.Lock()
	var left []*client
	for _, o := range rl.rooms[name] {
		if o != c {
			left = append(left, o)
		}
	}
	if len(left) == 0 {
		delete(rl.rooms, name)
	} else {
		rl.rooms[name] = left
	}
	rl.mu.Unlock()
	close(c.done)
	for _, o := range left {
		o.send([]byte(`{"type":"left"}`))
	}
}

const (
	opCont  = 0x0
	opText  = 0x1
	opClose = 0x8
	opPing  = 0x9
	opPong  = 0xa
)

// frame is what a client's writer sends: a message, or a control frame.
func (c *client) send(text []byte) { c.enqueue(append([]byte{opText}, text...)) }

func (c *client) control(op byte, payload []byte) { c.enqueue(append([]byte{op}, payload...)) }

func (c *client) enqueue(f []byte) {
	select {
	case c.out <- f:
	case <-c.done:
	}
}

func (c *client) write(conn net.Conn, w *bufio.Writer) {
	for {
		select {
		case f := <-c.out:
			writeFrame(w, f[0], f[1:])
			if w.Flush() != nil || f[0] == opClose {
				return
			}
		case <-c.done:
			return
		}
	}
}

// readMessage reads a whole message, its fragments joined, answering
// pings; a close is a message of its own.
func readMessage(r *bufio.Reader, c *client) (byte, []byte, error) {
	var op byte
	var msg []byte
	for {
		fin, o, payload, err := readFrame(r)
		if err != nil {
			return 0, nil, err
		}
		switch o {
		case opPing:
			c.control(opPong, payload)
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
		if len(msg) > 1<<20 {
			return 0, nil, errors.New("a message too large")
		}
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
	if h[1]&0x80 == 0 {
		err = errors.New("a client's frame without a mask")
		return
	}
	if n > 1<<20 {
		err = errors.New("a frame too large")
		return
	}
	var mask [4]byte
	if _, err = io.ReadFull(r, mask[:]); err != nil {
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
