package main

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/cbuild"
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
