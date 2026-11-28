package std

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/testserver"
)

// std/http's Swift (std/http/swift/Http.swift) on this Mac: its
// URLSession download, compiled with the generated glue's Response and
// Transport and a driver of the test's, against a local server: the
// progress as it comes, the file renamed into place, cancelling.
func TestHTTPSwiftDownload(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("URLSession: macOS")
	}
	if _, err := exec.LookPath("swiftc"); err != nil {
		t.Skip("no swiftc")
	}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/big":
			w.Header().Set("Content-Length", "1048576")
			chunk := strings.Repeat("a", 64*1024)
			for i := 0; i < 16; i++ {
				fmt.Fprint(w, chunk)
				w.(http.Flusher).Flush()
				time.Sleep(30 * time.Millisecond)
			}
		case "/stall":
			w.Header().Set("Content-Length", "10")
			fmt.Fprint(w, "abc")
			w.(http.Flusher).Flush()
			select {
			case <-r.Context().Done():
			case <-time.After(10 * time.Second):
			}
		}
	}))
	defer srv.Close()
	dir := t.TempDir()
	out := filepath.Join(dir, "big.bin")
	driver := `import Foundation
let t = try makeTransport()
var reports: [(Int, Int)] = []
let lock = NSLock()
let r = try await t.download(method: "GET", url: "` + srv.URL + `/big", headers: [:], body: Data(), timeout: 5, path: "` + out + `", progress: { got, total in
    lock.lock(); reports.append((got, total)); lock.unlock()
})
let size = (try FileManager.default.attributesOfItem(atPath: "` + out + `")[.size] as! NSNumber).intValue
lock.lock()
print("status \(r.status) body \(r.body.count) size \(size) reports \(reports.count > 1) last \(reports.last!.0) \(reports.last!.1) totals \(reports.allSatisfy { $0.1 == 1048576 })")
lock.unlock()
let stalled = Task {
    try await t.download(method: "GET", url: "` + srv.URL + `/stall", headers: [:], body: Data(), timeout: 0, path: "` + out + `.2", progress: { _, _ in })
}
try await Task.sleep(nanoseconds: 300_000_000)
stalled.cancel()
do {
    _ = try await stalled.value
    print("cancel: finished")
} catch {
    print("cancel: \((error as? URLError)?.code == .cancelled) \(FileManager.default.fileExists(atPath: "` + out + `.2"))")
}
`
	got := runHTTPProbe(t, dir, driver)
	for _, want := range []string{"status 200 body 0 size 1048576 reports true last 1048576 1048576 totals true", "cancel: true false"} {
		if !strings.Contains(got, want) {
			t.Errorf("want %q in:\n%s", want, got)
		}
	}
}

// goneServer serves testserver's paths, and /slowhead (a head that never
// comes), /trickle (its head and "a", then nothing) and /big (32 MiB in
// 64 KiB writes, the i-th all "a" + i%26), counting the requests whose
// client went before their answer ended: /gone?path=/stall says how many
// of that path's.
func goneServer() *httptest.Server {
	var mu sync.Mutex
	gone := map[string]int{}
	h := testserver.Handler()
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/gone":
			mu.Lock()
			fmt.Fprint(w, gone[r.URL.Query().Get("path")])
			mu.Unlock()
			return
		case "/slowhead":
			<-r.Context().Done()
		case "/trickle":
			w.Header().Set("Content-Type", "text/plain")
			fmt.Fprint(w, "a")
			w.(http.Flusher).Flush()
			<-r.Context().Done()
		case "/big":
			for i := 0; i < 512 && r.Context().Err() == nil; i++ {
				fmt.Fprint(w, strings.Repeat(string(rune('a'+i%26)), 64*1024))
				w.(http.Flusher).Flush()
			}
		default:
			h.ServeHTTP(w, r)
		}
		if r.Context().Err() != nil {
			mu.Lock()
			gone[r.URL.Path]++
			mu.Unlock()
		}
	}))
}

// std/http's streamed bodies in Swift (Transport.open, Body) on this Mac:
// the pieces as they come (the first well before the last), a 404 that
// still opens, a request's body and headers, a stall's timeout (for the
// head, then for a piece), a read
// cancelled (the request going on until closed), an open cancelled, a
// body let go, and a reader slower than the network (the task suspended
// past 1 MiB unread, and resumed: nothing lost, in order).
func TestHTTPSwiftStream(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("URLSession: macOS")
	}
	if _, err := exec.LookPath("swiftc"); err != nil {
		t.Skip("no swiftc")
	}
	srv := goneServer()
	defer srv.Close()
	driver := `import Foundation
let t = try makeTransport()
let base = "` + srv.URL + `"
func now() -> Double { Date().timeIntervalSince1970 }
func readAll(_ b: any Body) async throws -> Data {
    var all = Data()
    while true {
        let d = try await b.read()
        if d.isEmpty { return all }
        all.append(d)
    }
}
// Waits for the server to see path's requests gone (the client hung up).
func gone(_ path: String, _ want: Int, tries: Int = 200) async -> Bool {
    for _ in 0..<tries {
        if let (d, _) = try? await URLSession.shared.data(from: URL(string: base + "/gone?path=" + path)!), Int(String(decoding: d, as: UTF8.self)) ?? 0 >= want {
            return true
        }
        try? await Task.sleep(nanoseconds: 25_000_000)
    }
    return false
}

let b = try await t.open(method: "GET", url: base + "/stream", headers: [:], body: Data(), timeout: 5)
let h = try b.head()
var text = ""
var times: [Double] = []
while true {
    let d = try await b.read()
    if d.isEmpty { break }
    times.append(now())
    text += String(decoding: d, as: UTF8.self)
}
let end = now()
print("stream \(h.status) \(h.headers["content-type"] ?? "-") \(text.debugDescription) pieces \(times.count >= 2) spread \(times.last! - times.first! > 0.06) early \(end - times.first! > 0.08)")
print("stream timings first→last \(Int((times.last! - times.first!) * 1000)) ms, first→end \(Int((end - times.first!) * 1000)) ms, \(times.count) pieces")
print("after end: \(try await b.read().count)")

let nf = try await t.open(method: "GET", url: base + "/status/404", headers: [:], body: Data(), timeout: 5)
print("404: \(try nf.head().status) \(String(decoding: try await readAll(nf), as: UTF8.self))")

let echo = try await t.open(method: "POST", url: base + "/echo", headers: ["X-A": "b"], body: Data("hi".utf8), timeout: 5)
let j = try JSONSerialization.jsonObject(with: try await readAll(echo)) as! [String: Any]
print("echo: \(j["method"]!) \(j["body"]!) \((j["headers"] as! [String: String])["x-a"]!)")

// URLSession hands over a response's head with its body's first bytes
// (CFNetwork holds it till then): a head and nothing more times out in
// open there, a head and some in read.
let s0 = now()
do {
    let stall = try await t.open(method: "GET", url: base + "/stall", headers: [:], body: Data(), timeout: 0.5)
    _ = try await stall.read()
    print("stall: read")
} catch {
    print("stall: timedOut \((error as? URLError)?.code == .timedOut) soon \(now() - s0 < 5)")
}
print("stall gone: \(await gone("/stall", 1))")
let trickle = try await t.open(method: "GET", url: base + "/trickle", headers: [:], body: Data(), timeout: 0.5)
let s1 = now()
let first = try await trickle.read()
do {
    _ = try await trickle.read()
    print("trickle: read")
} catch {
    print("trickle: \(try trickle.head().status) \(String(decoding: first, as: UTF8.self)) timedOut \((error as? URLError)?.code == .timedOut) waited \(now() - s1 >= 0.4) soon \(now() - s1 < 5)")
}
print("trickle gone: \(await gone("/trickle", 1))")

let held = try await t.open(method: "GET", url: base + "/trickle", headers: [:], body: Data(), timeout: 0)
_ = try await held.read()
let reading = Task { try await held.read() }
try await Task.sleep(nanoseconds: 200_000_000)
reading.cancel()
do {
    _ = try await reading.value
    print("read cancel: read")
} catch {
    print("read cancel: \(error is CancellationError) still open \(!(await gone("/trickle", 2, tries: 12)))")
}
try held.close()
print("closed: gone \(await gone("/trickle", 2)) read \(try await held.read().count)")

let opening = Task { try await t.open(method: "GET", url: base + "/slowhead", headers: [:], body: Data(), timeout: 0) }
try await Task.sleep(nanoseconds: 200_000_000)
opening.cancel()
do {
    _ = try await opening.value
    print("open cancel: opened")
} catch {
    print("open cancel: \((error as? URLError)?.code == .cancelled) gone \(await gone("/slowhead", 1))")
}

do {
    let dropped = try await t.open(method: "GET", url: base + "/trickle", headers: [:], body: Data(), timeout: 0)
    _ = try dropped.head()
}
print("let go: gone \(await gone("/trickle", 3))")

let big = try await t.open(method: "GET", url: base + "/big", headers: [:], body: Data(), timeout: 5)
try await Task.sleep(nanoseconds: 500_000_000)
var count = 0
var inOrder = true
while true {
    let d = try await big.read()
    if d.isEmpty { break }
    for o in stride(from: 0, to: d.count, by: 4096) where d[d.startIndex + o] != UInt8(97 + ((count + o) >> 16) % 26) {
        inOrder = false
    }
    count += d.count
}
print("big: \(count) in order \(inOrder)")
`
	got := runHTTPProbe(t, t.TempDir(), driver)
	t.Logf("\n%s", got)
	for _, want := range []string{
		`stream 200 text/plain; charset=utf-8 "one\ntwo\nthree\n" pieces true spread true early true`,
		"after end: 0",
		"404: 404 missing",
		"echo: POST hi b",
		"stall: timedOut true soon true",
		"stall gone: true",
		"trickle: 200 a timedOut true waited true soon true",
		"trickle gone: true",
		"read cancel: true still open true",
		"closed: gone true read 0",
		"open cancel: true gone true",
		"let go: gone true",
		"big: 33554432 in order true",
	} {
		if !strings.Contains(got, want) {
			t.Errorf("want %q in:\n%s", want, got)
		}
	}
}

// runHTTPProbe compiles std/http's Swift with the generated glue's
// types (not its codec, which needs the runtime) and driver as main,
// runs it and gives what it printed.
func runHTTPProbe(t *testing.T, dir, driver string) string {
	return runSwiftProbe(t, dir, "http/swift/Http.asuka.swift", []string{"http/swift/Http.swift", "http/swift/TLS.swift", "http/swift/Upload.swift"}, []string{"Response", "Part", "FinishedUpload", "TLS", "Transport", "Body", "Sender", "LocalServer"}, driver)
}

// runSwiftProbe compiles a std package's Swift (impls) with the types of
// its generated glue named (structs and protocols, not its codec, which
// needs the runtime), a stand-in for module Asuka (what the package's
// Swift uses of it) and driver as main, runs it and gives what it
// printed.
func runSwiftProbe(t *testing.T, dir, gluePath string, impls, types []string, driver string) string {
	t.Helper()
	glue, err := os.ReadFile(gluePath)
	if err != nil {
		t.Fatal(err)
	}
	var shim strings.Builder
	shim.WriteString("import Foundation\n")
	for _, name := range types {
		re := `(?s)public (?:struct|protocol) ` + name + `[:\s][^{]*\{.*?\n\}\n`
		m := regexp.MustCompile(re).Find(glue)
		if m == nil {
			t.Fatalf("no %s in the glue", name)
		}
		shim.Write(m)
	}
	// Module Asuka as the package's Swift sees it: its errors, and the
	// hosts' background URLSessions' events (the hosts' own file).
	stub := filepath.Join(dir, "stub")
	os.MkdirAll(stub, 0o755)
	asuka := filepath.Join(stub, "Asuka.swift")
	sessions, err := os.ReadFile("../platform/ios/Sources/AsukaHost/BackgroundSessions.swift")
	if err != nil {
		t.Fatal(err)
	}
	os.WriteFile(asuka, append([]byte("public enum AsukaNativeError: Error { case unsupported(String) }\n"), sessions...), 0o644)
	if b, err := exec.Command("swiftc", "-parse-as-library", "-emit-module", "-module-name", "Asuka", "-emit-module-path", filepath.Join(stub, "Asuka.swiftmodule"), "-emit-object", "-o", filepath.Join(stub, "Asuka.o"), asuka).CombinedOutput(); err != nil {
		t.Fatalf("swiftc (Asuka): %v\n%s", err, b)
	}
	args := []string{"-swift-version", "5", "-I", stub}
	for name, src := range map[string]string{"shim.swift": shim.String(), "main.swift": driver} {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, []byte(src), 0o644); err != nil {
			t.Fatal(err)
		}
		args = append(args, p)
	}
	for _, impl := range impls {
		abs, _ := filepath.Abs(impl)
		args = append(args, abs)
	}
	exe := filepath.Join(dir, "probe")
	args = append(args, filepath.Join(stub, "Asuka.o"), "-o", exe)
	if b, err := exec.Command("swiftc", args...).CombinedOutput(); err != nil {
		t.Fatalf("swiftc: %v\n%s", err, b)
	}
	b, err := exec.Command(exe).CombinedOutput()
	if err != nil {
		t.Fatalf("%v\n%s", err, b)
	}
	return string(b)
}

// std/http's loopback server (Network.framework): a request to another
// path is a 404, the path's answers with the page and gives next its URL;
// close stops it.
func TestHTTPSwiftLoopback(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("Network.framework: macOS")
	}
	if _, err := exec.LookPath("swiftc"); err != nil {
		t.Skip("no swiftc")
	}
	driver := `import Foundation
let s = try makeLocalServer()
let port = try await s.listen(path: "/callback", page: "<p>Done</p>")
let base = "http://127.0.0.1:\(port)"
let waiting = Task { try await s.next(port: port) }
let (_, other) = try await URLSession.shared.data(from: URL(string: base + "/favicon.ico")!)
let (page, ok) = try await URLSession.shared.data(from: URL(string: base + "/callback?code=42&state=s")!)
let url = try await waiting.value
print("other \((other as! HTTPURLResponse).statusCode) ok \((ok as! HTTPURLResponse).statusCode) page \(String(decoding: page, as: UTF8.self)) url \(url == base + "/callback?code=42&state=s")")
let later = Task { try await s.next(port: port) }
try await Task.sleep(nanoseconds: 100_000_000)
later.cancel()
do { _ = try await later.value; print("cancel: answered") } catch { print("cancel: \(error is CancellationError)") }
try s.close(port: port)
try await Task.sleep(nanoseconds: 100_000_000)
do { _ = try await URLSession.shared.data(from: URL(string: base + "/callback")!); print("closed: answered") } catch { print("closed: refused") }
`
	got := runHTTPProbe(t, t.TempDir(), driver)
	for _, want := range []string{"other 404 ok 200 page <p>Done</p> url true", "cancel: true", "closed: refused"} {
		if !strings.Contains(got, want) {
			t.Errorf("want %q in:\n%s", want, got)
		}
	}
}

// std/http's TLS settings in Swift (TLS.swift) on this Mac, against
// internal/testserver over HTTPS with a test authority's certificate:
// the system's trust refusing it, the authority trusted alone (another
// refused, why said), with the system's, no check at all; a client
// certificate shown when asked for (EC as PKCS#8 and SEC 1, RSA as
// PKCS#1), imported in memory and, as before macOS 15, into a keychain
// of the process's own; the same settings the same transport; a stream
// and a download secured; PEM that can't be read.
func TestHTTPSwiftTLS(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("URLSession: macOS")
	}
	if _, err := exec.LookPath("swiftc"); err != nil {
		t.Skip("no swiftc")
	}
	pki, err := testserver.NewPKI()
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	if err := pki.WriteFiles(dir); err != nil {
		t.Fatal(err)
	}
	srv := testserver.StartTLS(testserver.Handler(), pki)
	defer srv.Close()
	driver := `import Foundation
let base = "` + srv.URL + `"
func read(_ n: String) -> String { try! String(contentsOfFile: "` + dir + `/" + n, encoding: .utf8) }
func said(_ e: Error) -> String { "error \((e as NSError).code) \(e.localizedDescription)" }
func get(_ t: any Transport, _ path: String = "/mtls") async -> String {
    do {
        let r = try await t.send(method: "GET", url: base + path, headers: [:], body: Data(), timeout: 10)
        return "\(r.status) \(String(decoding: r.body, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines))"
    } catch {
        return said(error)
    }
}
let plain = try makeTransport()
let ca = read("ca.pem")
print("system: \(await get(plain))")
let trusted = try await plain.secured(tls: TLS(ca: ca))
print("ca: \(await get(trusted))")
print("other: \(await get(try await plain.secured(tls: TLS(ca: read("other-ca.pem")))))")
print("both: \(await get(try await plain.secured(tls: TLS(ca: ca, trustSystem: true))))")
print("insecure: \(await get(try await plain.secured(tls: TLS(insecureSkipVerify: true))))")
print("ec: \(await get(try await plain.secured(tls: TLS(ca: ca, certificate: read("client.pem"), key: read("client.key")))))")
print("sec1: \(await get(try await plain.secured(tls: TLS(ca: ca, certificate: read("client.pem"), key: read("client-sec1.key")))))")
print("rsa: \(await get(try await plain.secured(tls: TLS(ca: ca, certificate: read("rsa-client.pem"), key: read("rsa-client.key")))))")
TLSPolicy.keychainImport = true
print("keychain: \(await get(try await plain.secured(tls: TLS(ca: ca, certificate: read("client.pem"), key: read("client.key") + "\n"))))")
TLSPolicy.keychainImport = false
print("same: \((try await plain.secured(tls: TLS(ca: ca)) as AnyObject) === (trusted as AnyObject))")
let b = try await trusted.open(method: "GET", url: base + "/stream", headers: [:], body: Data(), timeout: 10)
var all = Data()
while true {
    let d = try await b.read()
    if d.isEmpty { break }
    all.append(d)
}
print("stream: \(try b.head().status) \(String(decoding: all, as: UTF8.self).split(separator: "\n").joined(separator: " "))")
do {
    _ = try await plain.secured(tls: TLS(ca: read("other-ca.pem"))).open(method: "GET", url: base + "/stream", headers: [:], body: Data(), timeout: 10)
    print("stream other: opened")
} catch {
    print("stream other: \(said(error))")
}
let file = "` + dir + `/echo.json"
let d = try await trusted.download(method: "GET", url: base + "/echo", headers: [:], body: Data(), timeout: 10, path: file, progress: { _, _ in })
print("download: \(d.status) \(FileManager.default.fileExists(atPath: file))")
do {
    _ = try await plain.secured(tls: TLS(insecureSkipVerify: true)).download(method: "GET", url: "https://127.0.0.1:1/x", headers: [:], body: Data(), timeout: 10, path: file + ".2", progress: { _, _ in })
} catch {
    print("download refused: \((error as NSError).code)")
}
do {
    _ = try await plain.secured(tls: TLS(ca: "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n"))
    print("bad ca: made")
} catch {
    print("bad ca: \(error.localizedDescription)")
}
do {
    _ = try await plain.secured(tls: TLS(certificate: read("client.pem"), key: read("rsa-client.key")))
    print("mismatch: made")
} catch {
    print("mismatch: \(error.localizedDescription)")
}
// After the rest let the server in: the system's trust and another
// authority's still refuse it (no session of theirs resumed unchecked).
print("again: \(await get(plain)) | \(await get(try await plain.secured(tls: TLS(ca: read("other-ca.pem")))))")
`
	got := runHTTPProbe(t, t.TempDir(), driver)
	for _, want := range []string{
		"system: error -1202 ",
		"ca: 403 no client certificate",
		"other: error -1202 127.0.0.1's certificate isn't trusted: ",
		"both: 403 no client certificate",
		"insecure: 403 no client certificate",
		"ec: 200 client: Asuka Test Client",
		"sec1: 200 client: Asuka Test Client",
		"rsa: 200 client: Asuka Test RSA Client",
		"keychain: 200 client: Asuka Test Client",
		"same: true",
		"stream: 200 one two three",
		"stream other: error -1202 127.0.0.1's certificate isn't trusted: ",
		"download: 200 true",
		"download refused: -1004",
		"bad ca: tls: ca: a certificate can't be read",
		"mismatch: tls: the key can't be used with the certificate (it isn't its key, or can't be read): ",
		"again: error -1202 The certificate for this server is invalid.",
		" | error -1202 127.0.0.1's certificate isn't trusted: ",
	} {
		if !strings.Contains(got, want) {
			t.Errorf("want %q in:\n%s", want, got)
		}
	}
}

// brokenBodies wraps h, counting the requests whose body ended before its
// end (the client went): /broken says how many.
func brokenBodies(h http.Handler) http.Handler {
	var mu sync.Mutex
	broken := 0
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/broken" {
			mu.Lock()
			fmt.Fprint(w, broken)
			mu.Unlock()
			return
		}
		r.Body = &watchedBody{ReadCloser: r.Body, broke: func() { mu.Lock(); broken++; mu.Unlock() }}
		h.ServeHTTP(w, r)
	})
}

type watchedBody struct {
	io.ReadCloser
	broke func()
	once  sync.Once
}

func (b *watchedBody) Read(p []byte) (int, error) {
	n, err := b.ReadCloser.Read(p)
	if err != nil && err != io.EOF {
		b.once.Do(b.broke)
	}
	return n, err
}

// patterned writes size bytes at path, its i-th 64 KiB all byte(i*7),
// and gives their SHA-256.
func patterned(t *testing.T, path string, size int) string {
	t.Helper()
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	h := sha256.New()
	chunk := make([]byte, 64*1024)
	for i := 0; size > 0; i++ {
		n := min(size, len(chunk))
		for j := range chunk[:n] {
			chunk[j] = byte(i * 7)
		}
		f.Write(chunk[:n])
		h.Write(chunk[:n])
		size -= n
	}
	return hex.EncodeToString(h.Sum(nil))
}

// std/http's uploads in Swift (Upload.swift) on this Mac, against
// internal/testserver: a 256 MiB file from disk (its Content-Length, its
// bytes in order, progress as it goes and once at its end, the process's
// memory far under the file's size), a multipart form of bytes and files
// streamed (a big one too), bytes alone; a Sender's pieces chunked and
// with a length, a server that answers before the body's end, one closed
// (the request stopped); over TLS; background uploads in this process
// (a file, a form spooled, a secured one), one whose process went before
// its end (the next process's finished has it), the host's events
// reaching the session, and the finished store.
func TestHTTPSwiftUpload(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("URLSession: macOS")
	}
	if _, err := exec.LookPath("swiftc"); err != nil {
		t.Skip("no swiftc")
	}
	pki, err := testserver.NewPKI()
	if err != nil {
		t.Fatal(err)
	}
	h := brokenBodies(testserver.Handler())
	srv := httptest.NewServer(h)
	defer srv.Close()
	tlsSrv := testserver.StartTLS(h, pki)
	defer tlsSrv.Close()
	dir := t.TempDir()
	if err := pki.WriteFiles(dir); err != nil {
		t.Fatal(err)
	}
	const bigSize = 256 << 20
	big := filepath.Join(dir, "big.bin")
	bigSum := patterned(t, big, bigSize)
	const clipSize = 3<<20 + 12345
	clip := filepath.Join(dir, "clip.bin")
	clipSum := patterned(t, clip, clipSize)
	pieceSum := func(n int) string {
		h := sha256.New()
		for i := 0; i < n; i++ {
			h.Write(bytes.Repeat([]byte{byte(i * 7)}, 64*1024))
		}
		return hex.EncodeToString(h.Sum(nil))
	}
	driver := `import Foundation
import Asuka
setenv("ASUKA_DATA", "` + filepath.Join(dir, "data") + `", 1)
let base = "` + srv.URL + `"
let tlsBase = "` + tlsSrv.URL + `"
let dir = "` + dir + `"
let big = dir + "/big.bin", clip = dir + "/clip.bin"
let t = try makeTransport()
func size(_ p: String) -> Int { (try! FileManager.default.attributesOfItem(atPath: p)[.size] as! NSNumber).intValue }
func json(_ r: Response) -> [String: Any] { (try? JSONSerialization.jsonObject(with: r.body)) as? [String: Any] ?? ["body": String(decoding: r.body, as: UTF8.self)] }
func said(_ e: Error) -> String { "error \((e as NSError).code) \(e.localizedDescription)" }
func yes(_ v: Any?) -> String { (v as? Bool).map { "\($0)" } ?? "-" }
func rss() -> Int { var u = rusage(); getrusage(RUSAGE_SELF, &u); return Int(u.ru_maxrss) }
final class Reports: @unchecked Sendable {
    let lock = NSLock()
    var all: [(Int, Int)] = []
    func add(_ a: Int, _ b: Int) { lock.lock(); all.append((a, b)); lock.unlock() }
    func summary(_ total: Int) -> String {
        lock.lock(); defer { lock.unlock() }
        var mono = true
        for i in all.indices.dropFirst() where all[i].0 < all[i - 1].0 { mono = false }
        let last = all.last.map { "\($0.0) \($0.1)" } ?? "none"
        return "reports \(all.count > 1) monotonic \(mono) totals \(all.allSatisfy { $0.1 == total }) last \(last)"
    }
}
func piece(_ i: Int) -> Data { Data(repeating: UInt8(truncatingIfNeeded: i * 7), count: 64 * 1024) }
func form(_ b: String, _ file: String) -> ([Part], Int) {
    let head = Data("--\(b)\r\nContent-Disposition: form-data; name=\"title\"\r\n\r\nHoliday\r\n--\(b)\r\nContent-Disposition: form-data; name=\"video\"; filename=\"clip.bin\"\r\nContent-Type: video/mp4\r\n\r\n".utf8)
    let tail = Data("\r\n--\(b)\r\nContent-Disposition: form-data; name=\"note\"\r\n\r\nend\r\n--\(b)--\r\n".utf8)
    return ([Part(data: head), Part(path: file), Part(data: tail)], head.count + size(file) + tail.count)
}
func files(_ j: [String: Any]) -> String {
    let fs = (j["files"] as? [[String: Any]] ?? []).map { "\($0["name"]!) \($0["filename"]!) \($0["type"]!) \($0["bytes"]!) \($0["sha256"]!)" }
    let fields = (j["fields"] as? [String: String] ?? [:]).sorted { $0.key < $1.key }.map { "\($0.key)=\($0.value)" }
    return "fields \(fields.joined(separator: ",")) files \(fs.joined(separator: ";")) length \(j["length"] ?? "-") chunked \(yes(j["chunked"]))\(j["body"].map { " \($0)" } ?? "")"
}
func broken() async -> Int {
    let (d, _) = try! await URLSession.shared.data(from: URL(string: base + "/broken")!)
    return Int(String(decoding: d, as: UTF8.self)) ?? -1
}

if CommandLine.arguments.count > 1 && CommandLine.arguments[1] == "start" {
    // A background upload whose process goes before its end.
    let r = Reports()
    Task {
        _ = try? await t.upload(method: "POST", url: base + "/upload?delay=40", headers: ["Content-Type": "application/x-later"], parts: [Part(path: clip)], length: size(clip), timeout: 60, progress: r.add, background: true, id: "later")
    }
    while r.lock.withLock({ r.all.isEmpty }) { try await Task.sleep(nanoseconds: 10_000_000) }
    print("started")
    exit(0)
}
if CommandLine.arguments.count > 1 && CommandLine.arguments[1] == "collect" {
    for _ in 0..<300 {
        let f = try await t.finished()
        if !f.isEmpty {
            print("collected: " + f.map { "\($0.id) \($0.status) \($0.url.hasSuffix("/upload?delay=40")) \($0.headers["content-type"] ?? "-") error \($0.error.isEmpty) \($0.error)" }.joined(separator: "; "))
            print("again: \(try await t.finished().count) files \(BackgroundUploads.pending().count)")
            exit(0)
        }
        try await Task.sleep(nanoseconds: 100_000_000)
    }
    print("collected: none")
    exit(0)
}

// A file from disk.
let bigSize = size(big)
let r0 = Reports()
let f = try await t.upload(method: "PUT", url: base + "/upload", headers: ["Content-Type": "video/mp4"], parts: [Part(path: big)], length: bigSize, timeout: 30, progress: r0.add, background: false, id: "")
let fj = json(f)
print("file: \(f.status) \(fj["method"]!) \(fj["type"]!) bytes \(fj["bytes"]!) sha \(fj["sha256"]!) length \(fj["length"]!) chunked \(yes(fj["chunked"])) \(r0.summary(bigSize))")
print("file rss: \(rss() >> 20) MiB small \(rss() < 100 << 20)")

// A form: bytes, a file, bytes, streamed.
let (parts, length) = form("----asuka-test", clip)
let r1 = Reports()
let m = try await t.upload(method: "POST", url: base + "/multipart", headers: ["Content-Type": "multipart/form-data; boundary=----asuka-test"], parts: parts, length: length, timeout: 30, progress: r1.add, background: false, id: "")
print("form: \(m.status) \(files(json(m))) \(r1.summary(length)) length \(length)")
let (bigParts, bigLength) = form("----asuka-big", big)
let mb = try await t.upload(method: "POST", url: base + "/multipart", headers: ["Content-Type": "multipart/form-data; boundary=----asuka-big"], parts: bigParts, length: bigLength, timeout: 30, progress: { _, _ in }, background: false, id: "")
print("big form: \(mb.status) \(files(json(mb)))")
print("form rss: \(rss() >> 20) MiB small \(rss() < 100 << 20)")
let u = try await t.upload(method: "POST", url: base + "/multipart", headers: ["Content-Type": "multipart/form-data; boundary=----asuka-test"], parts: parts, length: -1, timeout: 30, progress: { _, _ in }, background: false, id: "")
print("form unknown: \(u.status) \(files(json(u)))")
do {
    _ = try await t.upload(method: "POST", url: base + "/upload", headers: [:], parts: [Part(data: Data("a".utf8)), Part(path: dir + "/missing")], length: -1, timeout: 30, progress: { _, _ in }, background: false, id: "")
    print("missing: sent")
} catch {
    print("missing: \((error as NSError).domain == NSCocoaErrorDomain) \(said(error))")
}

// Bytes alone.
let r2 = Reports()
let d = try await t.upload(method: "POST", url: base + "/upload", headers: ["Content-Type": "text/plain"], parts: [Part(data: Data("hello ".utf8)), Part(data: Data("world".utf8))], length: 11, timeout: 30, progress: r2.add, background: false, id: "")
let dj = json(d)
print("data: \(d.status) bytes \(dj["bytes"]!) length \(dj["length"]!) type \(dj["type"]!) last \(r2.all.last.map { "\($0.0) \($0.1)" } ?? "none")")

// A Sender's pieces: chunked, then with a length.
let r3 = Reports()
let s1 = try await t.begin(method: "POST", url: base + "/upload", headers: ["Content-Type": "application/x-pieces"], length: -1, timeout: 30, progress: r3.add)
var all = true
for i in 0..<40 { if !(try await s1.write(piece: piece(i))) { all = false } }
let a1 = try await s1.finish()
let j1 = json(a1)
print("chunked: \(a1.status) wrote \(all) bytes \(j1["bytes"]!) sha \(j1["sha256"]!) length \(j1["length"]!) chunked \(yes(j1["chunked"])) type \(j1["type"]!) \(r3.summary(-1))")
let r4 = Reports()
let s2 = try await t.begin(method: "PUT", url: base + "/upload?delay=5", headers: [:], length: 200 * 65536, timeout: 30, progress: r4.add)
for i in 0..<200 { _ = try await s2.write(piece: piece(i)) }
let a2 = try await s2.finish()
let j2 = json(a2)
print("sized: \(a2.status) \(j2["method"]!) bytes \(j2["bytes"]!) sha \(j2["sha256"]!) length \(j2["length"]!) chunked \(yes(j2["chunked"])) \(r4.summary(200 * 65536))")

// A server that answers before the body's end.
let s3 = try await t.begin(method: "POST", url: base + "/upload?answer=413", headers: [:], length: -1, timeout: 30, progress: { _, _ in })
var refused = -1
for i in 0..<2048 { if !(try await s3.write(piece: piece(i))) { refused = i; break } }
do {
    let a3 = try await s3.finish()
    print("early: \(a3.status) \(String(decoding: a3.body, as: UTF8.self)) write false \(refused >= 0) after \(refused)")
} catch {
    print("early: \(said(error)) write false \(refused >= 0) after \(refused)")
}

// Closed as it goes.
let before = await broken()
let s4 = try await t.begin(method: "POST", url: base + "/upload?delay=100", headers: [:], length: -1, timeout: 30, progress: { _, _ in })
_ = try await s4.write(piece: piece(0))
try await Task.sleep(nanoseconds: 300_000_000)
try s4.close()
let w4 = try await s4.write(piece: piece(1))
do {
    _ = try await s4.finish()
    print("closed: answered")
} catch {
    var stopped = false
    for _ in 0..<100 where !stopped {
        stopped = await broken() > before
        if !stopped { try await Task.sleep(nanoseconds: 50_000_000) }
    }
    print("closed: write \(w4) \((error as? URLError)?.code == .cancelled) stopped \(stopped)")
}
// An upload cancelled.
let cancelled = Task { try await t.upload(method: "POST", url: base + "/upload?delay=100", headers: [:], parts: [Part(path: clip)], length: size(clip), timeout: 30, progress: { _, _ in }, background: false, id: "") }
try await Task.sleep(nanoseconds: 300_000_000)
cancelled.cancel()
do { _ = try await cancelled.value; print("cancel: answered") } catch { print("cancel: \((error as? URLError)?.code == .cancelled)") }

// Over TLS.
func read(_ n: String) -> String { try! String(contentsOfFile: dir + "/" + n, encoding: .utf8) }
let secured = try await t.secured(tls: TLS(ca: read("ca.pem")))
let tf = try await secured.upload(method: "POST", url: tlsBase + "/upload", headers: [:], parts: [Part(path: clip)], length: size(clip), timeout: 30, progress: { _, _ in }, background: false, id: "")
print("tls file: \(tf.status) sha \(json(tf)["sha256"]!)")
let tm = try await secured.upload(method: "POST", url: tlsBase + "/multipart", headers: ["Content-Type": "multipart/form-data; boundary=----asuka-test"], parts: parts, length: length, timeout: 30, progress: { _, _ in }, background: false, id: "")
print("tls form: \(tm.status) \(files(json(tm)))")
let ts = try await secured.begin(method: "POST", url: tlsBase + "/upload", headers: [:], length: -1, timeout: 30, progress: { _, _ in })
for i in 0..<4 { _ = try await ts.write(piece: piece(i)) }
let tsa = try await ts.finish()
print("tls sender: \(tsa.status) bytes \(json(tsa)["bytes"]!)")
do {
    _ = try await t.secured(tls: TLS(ca: read("other-ca.pem"))).upload(method: "POST", url: tlsBase + "/upload", headers: [:], parts: [Part(path: clip)], length: size(clip), timeout: 30, progress: { _, _ in }, background: false, id: "")
    print("tls other: sent")
} catch {
    print("tls other: \(said(error))")
}

// Background, in this process.
let r5 = Reports()
let b1 = try await t.upload(method: "POST", url: base + "/upload?delay=1", headers: ["Content-Type": "video/mp4"], parts: [Part(path: big)], length: bigSize, timeout: 30, progress: r5.add, background: true, id: "b1")
let bj = json(b1)
print("background: \(b1.status) bytes \(bj["bytes"]!) sha \(bj["sha256"]!) type \(bj["type"]!) \(r5.summary(bigSize)) files \(BackgroundUploads.pending().count)")
let b2 = try await t.upload(method: "POST", url: base + "/multipart", headers: ["Content-Type": "multipart/form-data; boundary=----asuka-test"], parts: parts, length: length, timeout: 30, progress: { _, _ in }, background: true, id: "b2")
print("background form: \(b2.status) \(files(json(b2)))")
let b3 = try await secured.upload(method: "POST", url: tlsBase + "/upload", headers: [:], parts: [Part(data: Data("secret".utf8))], length: 6, timeout: 30, progress: { _, _ in }, background: true, id: "b3")
print("background tls: \(b3.status) bytes \(json(b3)["bytes"]!)")
print("background finished: \(try await t.finished().count) files \(BackgroundUploads.pending().count)")

// The host hands the session's events to it by its identifier.
var told = false
AsukaBackgroundSessions.handleEvents("x.AsukaNoSuchSession") { told = true }
AsukaBackgroundSessions.handleEvents(BackgroundUploads.identifier) { }
print("events: unknown done \(told) ours \(BackgroundUploads.identifier.hasSuffix(".AsukaHttpBackground")) connected \(BackgroundUploads.app.connected)")

// The finished store.
let store = FinishedStore(URL(fileURLWithPath: dir + "/store/finished.json"))
store.add(FinishedUpload(id: "a", status: 201, url: "https://x/a", headers: ["etag": "1"]))
store.add(FinishedUpload(id: "b", error: "lost"))
let again = FinishedStore(URL(fileURLWithPath: dir + "/store/finished.json")).take()
print("store: " + again.map { "\($0.id) \($0.status) \($0.url) \($0.headers) \($0.error)" }.joined(separator: "; ") + " then \(store.take().count)")
`
	got := runHTTPProbe(t, dir, driver)
	t.Logf("\n%s", got)
	formFiles := func(sum string, n int) string {
		return fmt.Sprintf("fields note=end,title=Holiday files video clip.bin video/mp4 %d %s", n, sum)
	}
	for _, want := range []string{
		fmt.Sprintf("file: 200 PUT video/mp4 bytes %d sha %s length %d chunked false reports true monotonic true totals true last %d %d", bigSize, bigSum, bigSize, bigSize, bigSize),
		"file rss: ",
		"form: 200 " + formFiles(clipSum, clipSize) + " length ",
		" chunked false reports ",
		"monotonic true totals true last 3158352 3158352 length 3158352",
		"big form: 200 " + formFiles(bigSum, bigSize),
		"form unknown: 200 " + formFiles(clipSum, clipSize) + " length -1 chunked true",
		"missing: true",
		"data: 200 bytes 11 length 11 type text/plain last 11 11",
		"chunked: 200 wrote true bytes 2621440 sha " + pieceSum(40) + " length -1 chunked true type application/x-pieces reports true monotonic true totals true last 2621440 -1",
		"sized: 200 PUT bytes 13107200 sha " + pieceSum(200) + " length 13107200 chunked false reports true monotonic true totals true last 13107200 13107200",
		"early: 413 refused write false true",
		"closed: write false true stopped true",
		"cancel: true",
		"tls file: 200 sha " + clipSum,
		"tls form: 200 " + formFiles(clipSum, clipSize),
		"tls sender: 200 bytes 262144",
		"tls other: error -1202 127.0.0.1's certificate isn't trusted: ",
		fmt.Sprintf("background: 200 bytes %d sha %s type video/mp4 reports true monotonic true totals true last %d %d files 0", bigSize, bigSum, bigSize, bigSize),
		"background form: 200 " + formFiles(clipSum, clipSize),
		"background tls: 200 bytes 6",
		"background finished: 0 files 0",
		"events: unknown done true ours true connected true",
		`store: a 201 https://x/a ["etag": "1"] ; b 0  [:] lost then 0`,
	} {
		if !strings.Contains(got, want) {
			t.Errorf("want %q in:\n%s", want, got)
		}
	}
	for _, line := range strings.Split(got, "\n") {
		if strings.HasSuffix(line, "small false") {
			t.Errorf("the process's memory grew with the file: %s", line)
		}
	}
	// A background upload whose process went before its end: the next
	// process's finished has it, once.
	exe := filepath.Join(dir, "probe")
	if b, err := exec.Command(exe, "start").CombinedOutput(); err != nil || !strings.Contains(string(b), "started") {
		t.Fatalf("start: %v\n%s", err, b)
	}
	b, err := exec.Command(exe, "collect").CombinedOutput()
	if err != nil {
		t.Fatalf("collect: %v\n%s", err, b)
	}
	t.Logf("\n%s", b)
	for _, want := range []string{"collected: later 200 true application/json error true", "again: 0 files 0"} {
		if !strings.Contains(string(b), want) {
			t.Errorf("want %q in:\n%s", want, b)
		}
	}
}
