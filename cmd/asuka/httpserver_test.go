package main

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/internal/testserver"
)

// A program that serves HTTP and calls itself with std/http's own
// client, built for this computer: URLSession on macOS, libsoup on
// Linux, WinHTTP on Windows against the core's server. JSON both ways, a
// body read as it streams, server-sent events, the router's 404 and
// 405, many requests at once, and a close that waits for them.
func TestProgramHTTPServer(t *testing.T) {
	if _, err := cbuild.Find(); err != nil {
		t.Skip(err)
	}
	dir := filepath.Join(t.TempDir(), "notes")
	os.MkdirAll(dir, 0o755)
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("name = \"notes\"\nversion = \"0.1.0\"\n"), 0o644)
	main := `import std/http

struct Note {
  id: int
  text: string
}

async fn main() throws {
  var notes = [Note(id: 1, text: "milk")]
  let api = http.Router()
    .get("/notes/:id", req => {
      if let n = notes.find(n => n.id == req.param("id").toInt()) {
        return .json(n)
      }
      return .text("no note", status: 404)
    })
    .post("/notes", req => {
      var n = try req.json<Note>()
      n.id = notes.count() + 1
      notes.push(n)
      return .json(n, status: 201)
    })
    .get("/count", req => .stream(Stream<bytes>(send => {
      for i in 1..3 {
        send("line {i}\n".bytes())
        await sleep(10ms)
      }
    }), type: "text/plain"))
    .get("/events", req => .events(Stream<http.Event>(send => {
      send(http.Event(data: "hello"))
      send(http.Event(event: "price", data: "42", id: "7"))
    })))
  let server = try await http.serve(0, api.handler())
  do {
    let _ = try await http.get("{server.url}/notes/1")
  } catch e {
    print("no client: {e.message}")
    await server.close()
    return
  }
  let r = try await http.get("{server.url}/notes/1")
  print("{r.status} {r.text()} {r.headers["content-type"] ?? ""}")
  let p = try await http.postJSON("{server.url}/notes", value: Note(id: 0, text: "tea"))
  print("{p.status} {p.text()}")
  let s = try await http.stream("{server.url}/count")
  for await l in s.lines() {
    print(l)
  }
  for await e in http.events("{server.url}/events") {
    print("{e.event}|{e.data}|{e.id}")
  }
  print((try await http.get("{server.url}/notes/9")).status)
  let m = try await http.request("PUT", "{server.url}/notes/1")
  print("{m.status} {m.headers["allow"] ?? ""}")
  var urls: [string] = []
  for i in 1..20 {
    urls.push("{server.url}/notes/{(i % 2) + 1}")
  }
  let codes = try await all(urls, u => (try await http.get(u)).status)
  print(codes.all(c => c == 200))
  await server.close()
  print("closed")
}
`
	os.WriteFile(filepath.Join(dir, "main.asuka"), []byte(main), 0o644)
	p, err := appbuild.Load(dir)
	if err != nil {
		t.Fatal(err)
	}
	o := buildOptions{out: filepath.Join(t.TempDir(), "dist")}
	if err := cmdBuildProgram(p, &o); err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	cmd := exec.Command(filepath.Join(o.out, programExeName("notes", cbuild.Host())))
	cmd.Stdout, cmd.Stderr = &out, &out
	if err := cmd.Run(); err != nil {
		t.Fatalf("%v\n%s", err, out.String())
	}
	got := strings.ReplaceAll(out.String(), "\r\n", "\n")
	if strings.HasPrefix(got, "no client: ") && runtime.GOOS == "linux" {
		t.Skip("no libsoup here: ", strings.TrimSpace(got))
	}
	want := `200 {"id":1,"text":"milk"} application/json
201 {"id":2,"text":"tea"}
line 1
line 2
line 3
message|hello|
price|42|7
404
405 GET, HEAD
true
closed
`
	if got != want {
		t.Fatalf("got:\n%s\nwant:\n%s", got, want)
	}
}

// A program whose packages have platform code (std/http: Swift on the
// Mac, which runs it on the main queue's loop) and whose async main ends
// without awaiting anything exits: its end was ready, not
// posted, and the host's loop calls back only for what's posted.
func TestProgramAsyncMainEndsAtOnce(t *testing.T) {
	if _, err := cbuild.Find(); err != nil {
		t.Skip(err)
	}
	dir := filepath.Join(t.TempDir(), "quick")
	os.MkdirAll(dir, 0o755)
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("name = \"quick\"\nversion = \"0.1.0\"\n"), 0o644)
	os.WriteFile(filepath.Join(dir, "main.asuka"), []byte(`import std/http
import std/os

async fn main() throws {
  if os.args().count() > 5 {
    let _ = try await http.get("http://127.0.0.1:1/")
  }
  print("done")
}
`), 0o644)
	p, err := appbuild.Load(dir)
	if err != nil {
		t.Fatal(err)
	}
	o := buildOptions{out: filepath.Join(t.TempDir(), "dist")}
	if err := cmdBuildProgram(p, &o); err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	cmd := exec.Command(filepath.Join(o.out, programExeName("quick", cbuild.Host())))
	cmd.Stdout, cmd.Stderr = &out, &out
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	select {
	case err := <-done:
		if code := cmd.ProcessState.ExitCode(); code != 0 || strings.TrimSpace(out.String()) != "done" {
			t.Fatalf("exit %d (%v), output %q", code, err, out.String())
		}
	case <-time.After(60 * time.Second):
		cmd.Process.Kill()
		t.Fatalf("it didn't exit; output %q", out.String())
	}
}

// A program that serves HTTPS (net.serverTLS, http.serve's tls:) with a
// test authority's certificates (testserver.NewPKI's, made for the run:
// Apple's systems trust no server certificate valid for more than 825
// days, so the TLS tests' committed ones, valid until 2100, won't do) and
// calls itself with std/http's client and its TLS settings, built for
// this computer: URLSession on macOS, libsoup on Linux, WinHTTP on
// Windows. The test root trusted (alone, with the system's), not trusted
// by the system nor by another root, nothing checked; a server that asks
// for a client's certificate answering the one shown and refusing none;
// a stream and a download secured; settings that can't be read; and the
// system's trust still refusing once the others let the server in.
func TestProgramHTTPSClient(t *testing.T) {
	if _, err := cbuild.Find(); err != nil {
		t.Skip(err)
	}
	dir := filepath.Join(t.TempDir(), "secure")
	os.MkdirAll(dir, 0o755)
	pki, err := testserver.NewPKI()
	if err != nil {
		t.Fatal(err)
	}
	if err := pki.WriteFiles(dir); err != nil {
		t.Fatal(err)
	}
	os.WriteFile(filepath.Join(dir, "server.pem"), []byte(pki.ServerCert), 0o644)
	os.WriteFile(filepath.Join(dir, "server.key"), []byte(pki.ServerKey), 0o644)
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("name = \"secure\"\nversion = \"0.1.0\"\n"), 0o644)
	main := `import std/fs
import std/http
import std/net

async fn get(url: string, tls: http.TLS?) -> string {
  do {
    let r = try await http.get(url, tls: tls)
    return "{r.status} {r.text()}"
  } catch e {
    return if e.message.contains("certificate isn't trusted") { "untrusted" } else { "refused" }
  }
}

async fn main() throws {
  let ca = try fs.readText("ca.pem")
  let cert = try fs.readText("server.pem")
  let key = try fs.readText("server.key")
  let mine = http.TLS(ca: ca, certificate: try fs.readText("client.pem"), key: try fs.readText("client.key"))
  let api = http.Router()
    .get("/hello", req => .text("hello"))
    .get("/count", req => .stream(Stream<bytes>(send => {
      for i in 1..3 {
        send("line {i}\n".bytes())
      }
    }), type: "text/plain"))
  let open = try await http.serve(0, api.handler(), tls: try net.serverTLS(certificate: cert, key: key))
  let mutual = try await http.serve(0, api.handler(), tls: try net.serverTLS(certificate: cert, key: key, clientCA: ca))
  do {
    let _ = try await http.get("{open.url}/hello", tls: http.TLS(insecureSkipVerify: true))
  } catch e {
    print("no client: {e.message}")
    await open.close()
    await mutual.close()
    return
  }
  print("system: {await get("{open.url}/hello", none)}")
  print("ca: {await get("{open.url}/hello", http.TLS(ca: ca))}")
  print("other: {await get("{open.url}/hello", http.TLS(ca: try fs.readText("other-ca.pem")))}")
  print("both: {await get("{open.url}/hello", http.TLS(ca: ca, trustSystem: true))}")
  print("insecure: {await get("{open.url}/hello", http.TLS(insecureSkipVerify: true))}")
  print("mutual, none shown: {await get("{mutual.url}/hello", http.TLS(ca: ca))}")
  print("mutual: {await get("{mutual.url}/hello", mine)}")
  let s = try await http.stream("{mutual.url}/count", tls: mine)
  var lines: [string] = []
  for await l in s.lines() {
    lines.push(l)
  }
  print("stream: {s.status} {lines}")
  let d = try await http.download("{open.url}/hello", "hello.txt", tls: http.TLS(ca: ca))
  print("download: {d.status} {try fs.readText("hello.txt")}")
  do {
    let _ = try await http.get("{open.url}/hello", tls: http.TLS(certificate: try fs.readText("client.pem")))
  } catch http.HTTPError.tls(reason) {
    print("bad: {reason}")
  }
  // After the rest let the server in, the system's trust and another
  // authority's still refuse it: no session of theirs resumed unchecked.
  print("again: {await get("{open.url}/hello", none)} {await get("{open.url}/hello", http.TLS(ca: try fs.readText("other-ca.pem")))}")
  await open.close()
  await mutual.close()
}
`
	os.WriteFile(filepath.Join(dir, "main.asuka"), []byte(main), 0o644)
	p, err := appbuild.Load(dir)
	if err != nil {
		t.Fatal(err)
	}
	o := buildOptions{out: filepath.Join(t.TempDir(), "dist")}
	if err := cmdBuildProgram(p, &o); err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	cmd := exec.Command(filepath.Join(o.out, programExeName("secure", cbuild.Host())))
	cmd.Dir = dir
	cmd.Stdout, cmd.Stderr = &out, &out
	if err := cmd.Run(); err != nil {
		t.Fatalf("%v\n%s", err, out.String())
	}
	got := strings.ReplaceAll(out.String(), "\r\n", "\n")
	if strings.HasPrefix(got, "no client: ") && runtime.GOOS == "linux" {
		t.Skip("no libsoup here: ", strings.TrimSpace(got))
	}
	want := `system: refused
ca: 200 hello
other: untrusted
both: 200 hello
insecure: 200 hello
mutual, none shown: refused
mutual: 200 hello
stream: 200 ["line 1", "line 2", "line 3"]
download: 200 hello
bad: a client certificate needs its key (key:)
again: refused untrusted
`
	if got != want {
		t.Fatalf("got:\n%s\nwant:\n%s", got, want)
	}
}
