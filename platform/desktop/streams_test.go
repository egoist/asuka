package desktop_test

import (
	"crypto/sha256"
	"encoding/hex"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/testserver"
)

// streams serves internal/testserver for TestStdLinux's std/http stream,
// upload, std/websocket and TLS cases (testdata/streams; over HTTPS too,
// with a test authority's certificate), counting the /stall and /upload
// requests still being served: a body that timed out, or whose read was
// cancelled, has its request stopped, and so does an upload cancelled.
type streams struct {
	srv    *httptest.Server
	tls    *httptest.Server // the same paths over HTTPS, pki's certificate
	pki    *testserver.PKI
	stalls atomic.Int32
	// The /upload and /multipart requests being served.
	uploads atomic.Int32
}

func newStreams(t *testing.T) *streams {
	s := &streams{}
	pki, err := testserver.NewPKI()
	if err != nil {
		t.Fatal(err)
	}
	s.pki = pki
	s.tls = testserver.StartTLS(testserver.Handler(), pki)
	t.Cleanup(s.tls.Close)
	h := testserver.Handler()
	s.srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/stall" {
			s.stalls.Add(1)
			defer s.stalls.Add(-1)
		}
		if r.URL.Path == "/upload" || r.URL.Path == "/multipart" {
			s.uploads.Add(1)
			defer s.uploads.Add(-1)
		}
		h.ServeHTTP(w, r)
	}))
	t.Cleanup(s.srv.Close)
	return s
}

// source is testdata/streams/<name>, its server's URLs filled in.
func (s *streams) source(t *testing.T, name string) string {
	b, err := os.ReadFile(filepath.Join("testdata", "streams", name))
	if err != nil {
		t.Fatal(err)
	}
	return strings.NewReplacer(append([]string{`"BASE"`, `"` + s.srv.URL + `"`, `"WSBASE"`, `"ws` + strings.TrimPrefix(s.srv.URL, "http") + `"`, `"DONE_FILE"`, `""`,
		`"BIG_SHA"`, strconv.Quote(bigUploadSHA())},
		tlsPlaceholders(s.tls.URL, s.pki)...)...).Replace(string(b))
}

// bigUploadSHA is the SHA-256 of the file testdata/streams/upload.asuka
// makes and uploads: "0123456789abcdef" over 256 MiB.
var bigUploadSHA = sync.OnceValue(func() string {
	h := sha256.New()
	mib := []byte(strings.Repeat("0123456789abcdef", 65536))
	for i := 0; i < 256; i++ {
		h.Write(mib)
	}
	return hex.EncodeToString(h.Sum(nil))
})

// uploaded checks, once the upload app is done, that no upload it
// cancelled is still being served (what it shows says the rest: its
// memory, measured by the app itself as it uploads).
func (s *streams) uploaded(t *testing.T, pid int, line string) {
	if strings.HasPrefix(line, "host: grew") {
		t.Log(line)
	}
	if line != "host: done" {
		return
	}
	for end := time.Now().Add(10 * time.Second); s.uploads.Load() > 0; time.Sleep(20 * time.Millisecond) {
		if time.Now().After(end) {
			t.Errorf("%d uploads still served after they were cancelled", s.uploads.Load())
			return
		}
	}
}

// tlsPlaceholders are testdata/streams/tls.asuka's placeholders and what
// they become for a server at base (https://...) with pki's certificate:
// pairs for strings.NewReplacer.
func tlsPlaceholders(base string, pki *testserver.PKI) []string {
	return []string{`"TLS_BASE"`, strconv.Quote(base), `"WSS_BASE"`, strconv.Quote("wss" + strings.TrimPrefix(base, "https")),
		`"TLS_CA"`, strconv.Quote(pki.CA), `"TLS_OTHER_CA"`, strconv.Quote(pki.OtherCA),
		`"TLS_CLIENT_CERT"`, strconv.Quote(pki.ClientCert), `"TLS_CLIENT_KEY"`, strconv.Quote(pki.ClientKey)}
}

// stopped checks, once the app says it cancelled a read of /stall, that
// the requests to /stall stop (the server holds each 30 s otherwise).
func (s *streams) stopped(t *testing.T, pid int, line string) {
	if !strings.HasPrefix(line, "host: cancelled") {
		return
	}
	for end := time.Now().Add(10 * time.Second); s.stalls.Load() > 0; {
		if time.Now().After(end) {
			t.Errorf("%d requests to /stall still served after their bodies timed out or were cancelled", s.stalls.Load())
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
}

// What testdata/streams' apps show, on every desktop.
var (
	streamsHTTP = []string{
		"stream 200 text/plain; charset=utf-8 first one| all one|two|three|",
		"lines <one><two><three>",
		"events message:first: price:1+2:2 message:last:2",
		"posted 200 true true true",
		"long true false",
		"missing 404 false missing",
		"stall timed out true",
		"cancelled 200",
		"again one|two|three|",
		"done",
	}
	streamsUpload = []string{
		"made 268435456",
		"big 200 true true true true true true",
		"memory true",
		"slow 200 true true true true",
		"multipart 200 true true true true true",
		"chunked 200 true true true true true 1000000 1000000",
		"sized 200 true true true true",
		"short true",
		"early 413 refused",
		"early stream 413 refused",
		"early big 413 refused",
		"cancelled file",
		"cancelled stream",
		"again 200 true",
		"finished 0",
		"done",
	}
	streamsTLS = []string{
		"http refused 403 untrusted 403 403",
		"mutual 200 client: Asuka Test Client",
		"wss refused hello untrusted hello",
		"again refused untrusted refused",
		"done",
	}
	streamsWebSocket = []string{
		"echo chat: header:t1 hello binary bin closed 4000 bye",
		"normal [] ended",
		"client still ended, then refused",
		"refused yes",
		"done",
	}
)

func hostLines(lines []string) []string {
	out := make([]string, len(lines))
	for i, l := range lines {
		out[i] = "host: " + l
	}
	return out
}
