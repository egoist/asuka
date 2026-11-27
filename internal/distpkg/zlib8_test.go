package distpkg

import (
	"bytes"
	"compress/zlib"
	"fmt"
	"io"
	"math/rand/v2"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"github.com/egoist/asuka/internal/cbuild"
)

// zlib8Corpus is what Zlib8 is checked on: what doesn't compress, what
// compresses well, runs, text, code as an executable has it, and the
// sizes at the edges of flate.c's blocks and lookahead.
func zlib8Corpus(t *testing.T) map[string][]byte {
	r := rand.New(rand.NewPCG(8, 8))
	random := make([]byte, 128<<10)
	for i := range random {
		random[i] = byte(r.Uint32())
	}
	small := make([]byte, 128<<10) // few symbols: short codes, long matches
	for i := range small {
		small[i] = byte(r.IntN(6) * 7)
	}
	var text bytes.Buffer
	words := []string{"the", "app", "update", "AppImage", "delta", "block", "of", "a", "is", "and", "SquashFS", "\n"}
	for text.Len() < 200<<10 {
		text.WriteString(words[r.IntN(len(words))])
		text.WriteByte(' ')
	}
	// Code: this test's own executable, as much as a block holds, from
	// past its header.
	exe, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	code, err := os.ReadFile(exe)
	if err != nil {
		t.Fatal(err)
	}
	c := map[string][]byte{
		"empty":    nil,
		"one":      {'a'},
		"four":     []byte("abab"),
		"short":    []byte("hello, hello, hello, world"),
		"zeros":    make([]byte, 128<<10),
		"random":   random,
		"small":    small,
		"text":     text.Bytes()[:128<<10],
		"text256":  text.Bytes()[:256<<10-1],
		"look":     text.Bytes()[:262],
		"look+1":   text.Bytes()[:263],
		"code":     code[4096 : 4096+128<<10],
		"code2":    code[len(code)/2 : len(code)/2+128<<10],
		"codetail": code[len(code)/3 : len(code)/3+70000],
	}
	// A block of code with runs of literals long enough to skip (the
	// level's chain, 64, and more).
	mixed := append(append(append([]byte{}, random[:5000]...), text.Bytes()[:5000]...), random[5000:9000]...)
	c["mixed"] = append(mixed, mixed...)
	return c
}

// Zlib8 is a zlib stream at level 8 that zlib reads back: everything in
// the corpus, as it was.
func TestZlib8RoundTrip(t *testing.T) {
	for name, data := range zlib8Corpus(t) {
		z := Zlib8(data)
		zr, err := zlib.NewReader(bytes.NewReader(z))
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		got, err := io.ReadAll(zr)
		if err != nil || !bytes.Equal(got, data) {
			t.Errorf("%s: read back %d bytes, %v; want %d", name, len(got), err, len(data))
		}
		if !bytes.HasPrefix(z, []byte{0x78, 0xDA}) {
			t.Errorf("%s: its header is % x", name, z[:2])
		}
	}
}

// Zlib8 makes the bytes the core's deflater makes at level 8 (flate.c,
// framed as compress.c's zlib): what an app updating its AppImage from a
// delta compresses its blocks with. The core's C is built with this
// computer's compiler and run on the corpus.
func TestZlib8MatchesCore(t *testing.T) {
	cc, err := cbuild.Find()
	if err != nil {
		t.Skip(err)
	}
	core, err := cbuild.CoreDir()
	if err != nil {
		t.Skip(err)
	}
	dir := t.TempDir()
	main := filepath.Join(dir, "zlib8.c")
	// Each file named on the command line, compressed into <file>.z as
	// compress.c's ak_cz_deflate does at level 8 (header, the deflater
	// written once, finished, Adler-32).
	src := `#include "std/flate.h"
#include <stdio.h>
#include <stdlib.h>
void *ak_alloc(size_t n) { return calloc(1, n); }
void *ak_realloc(void *p, size_t n) { return realloc(p, n); }
void ak_free(void *p) { free(p); }
static bool put(void *u, const uint8_t *p, size_t n) { return fwrite(p, 1, n, u) == n; }
int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		FILE *in = fopen(argv[i], "rb");
		fseek(in, 0, SEEK_END);
		long n = ftell(in);
		fseek(in, 0, SEEK_SET);
		uint8_t *b = malloc(n ? (size_t)n : 1);
		if (fread(b, 1, (size_t)n, in) != (size_t)n) return 1;
		fclose(in);
		char path[4096];
		snprintf(path, sizeof path, "%s.z", argv[i]);
		FILE *out = fopen(path, "wb");
		fputc(0x78, out), fputc(0xDA, out);
		ak_deflater *d = ak_deflater_new(8, put, out);
		ak_deflater_write(d, b, (size_t)n);
		ak_deflater_finish(d);
		ak_deflater_free(d);
		uint32_t a = 1, s1 = 1, s2 = 0;
		for (long k = 0; k < n; k++) s1 = (s1 + b[k]) % 65521, s2 = (s2 + s1) % 65521;
		a = s2 << 16 | s1;
		uint8_t t[4] = {a >> 24, a >> 16, a >> 8, a};
		fwrite(t, 1, 4, out);
		fclose(out);
		free(b);
	}
	return 0;
}
`
	if err := os.WriteFile(main, []byte(src), 0o644); err != nil {
		t.Fatal(err)
	}
	exe := filepath.Join(dir, "zlib8")
	args := append(append([]string{}, cc.Args...), "-std=c17", "-O2", "-I", filepath.Join(core, "src"), "-I", filepath.Join(core, "include"),
		main, filepath.Join(core, "src", "std", "flate.c"), "-o", exe)
	if out, err := exec.Command(cc.Path, args...).CombinedOutput(); err != nil {
		t.Fatalf("building flate.c: %v\n%s", err, out)
	}
	corpus := zlib8Corpus(t)
	var files []string
	for name, data := range corpus {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, data, 0o644); err != nil {
			t.Fatal(err)
		}
		files = append(files, p)
	}
	if out, err := exec.Command(exe, files...).CombinedOutput(); err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	for name, data := range corpus {
		want, err := os.ReadFile(filepath.Join(dir, name+".z"))
		if err != nil {
			t.Fatal(err)
		}
		if got := Zlib8(data); !bytes.Equal(got, want) {
			t.Errorf("%s (%d bytes): Zlib8 made %d bytes, the core %d; first difference at %s", name, len(data), len(got), len(want), firstDiff(got, want))
		}
	}
}

func firstDiff(a, b []byte) string {
	for i := range min(len(a), len(b)) {
		if a[i] != b[i] {
			return fmt.Sprint(i)
		}
	}
	return fmt.Sprint(min(len(a), len(b)))
}
