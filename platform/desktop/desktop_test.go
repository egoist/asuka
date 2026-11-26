// The shared desktop C's tests (test/desktop_test.c), built with this
// computer's compiler and run here: what platform/windows and
// platform/linux share, checked on any system.
package desktop_test

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"image"
	"image/color"
	"image/gif"
	"image/png"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/animimage"
	"github.com/egoist/asuka/internal/cbuild"
)

func TestC(t *testing.T) {
	cc, err := cbuild.Find()
	if err != nil {
		t.Skip(err)
	}
	// desktop.h's sources (the registry's and packages' shared C are
	// asuka_desktop.h's, tested by TestNative and TestStd).
	all, _ := filepath.Glob("src/*.c")
	var srcs []string
	for _, f := range all {
		if b, _ := os.ReadFile(f); bytes.Contains(b, []byte(`#include "desktop.h"`)) || bytes.Contains(b, []byte(`#include "words.h"`)) {
			srcs = append(srcs, f)
		}
	}
	srcs = append(srcs, "test/desktop_test.c")
	exe := filepath.Join(t.TempDir(), "desktop_test")
	args := append(append([]string{}, cc.Args...), cbuild.Flags...)
	args = append(args, "-Iinclude", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined")
	args = append(args, srcs...)
	args = append(args, "-o", exe)
	if out, err := exec.Command(cc.Path, args...).CombinedOutput(); err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	dir := t.TempDir()
	writeAnimations(t, dir)
	out, err := exec.Command(exe, dir).CombinedOutput()
	if err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
}

// writeAnimations writes the animated images test_anim decodes, as
// internal/animimage's tests make them: anim.gif (image/gif's), anim.png
// (an APNG of image/png's encodings) and anim.webp (frames of fake
// bitstreams, a VP8L chunk of one gray byte).
func writeAnimations(t *testing.T, dir string) {
	red, blue := color.NRGBA{255, 0, 0, 255}, color.NRGBA{0, 0, 255, 255}
	pal := color.Palette{color.NRGBA{}, red, blue}
	f0 := image.NewPaletted(image.Rect(0, 0, 4, 4), pal)
	for i := range f0.Pix {
		f0.Pix[i] = 1
	}
	f1 := image.NewPaletted(image.Rect(2, 2, 4, 4), pal)
	for i := range f1.Pix {
		f1.Pix[i] = 2
	}
	f1.Pix[0] = 0 // transparent: frame 0 shows through
	var g bytes.Buffer
	if err := gif.EncodeAll(&g, &gif.GIF{Image: []*image.Paletted{f0, f1}, Delay: []int{5, 0}, Disposal: []byte{0, gif.DisposalBackground},
		LoopCount: 2, Config: image.Config{Width: 4, Height: 4}}); err != nil {
		t.Fatal(err)
	}
	chunk := func(b []byte, typ string, data []byte) []byte {
		b = binary.BigEndian.AppendUint32(b, uint32(len(data)))
		start := len(b)
		b = append(append(b, typ...), data...)
		return binary.BigEndian.AppendUint32(b, crc32.ChecksumIEEE(b[start:]))
	}
	// An APNG: a 4×4 default image, then a 2×2 frame at 1, 1, blended and
	// disposed to the previous.
	a := []byte("\x89PNG\r\n\x1a\n")
	seq := uint32(0)
	for i, r := range []image.Rectangle{image.Rect(0, 0, 4, 4), image.Rect(1, 1, 3, 3)} {
		img := image.NewNRGBA(r)
		for j := range img.Pix {
			img.Pix[j] = 200
		}
		var b bytes.Buffer
		if err := png.Encode(&b, img); err != nil {
			t.Fatal(err)
		}
		fc := binary.BigEndian.AppendUint32(nil, seq)
		seq++
		for _, v := range []int{r.Dx(), r.Dy(), r.Min.X, r.Min.Y} {
			fc = binary.BigEndian.AppendUint32(fc, uint32(v))
		}
		fc = binary.BigEndian.AppendUint16(fc, 3)
		fc = binary.BigEndian.AppendUint16(fc, 100) // 30 ms
		fc = append(fc, byte(2*i), byte(i))         // dispose: none, previous; blend: source, over
		d := b.Bytes()
		for p := 8; p+12 <= len(d); {
			n := int(binary.BigEndian.Uint32(d[p:]))
			typ, data := string(d[p+4:p+8]), d[p+8:p+8+n]
			switch {
			case typ == "IHDR" && i == 0:
				a = chunk(a, "IHDR", data)
				actl := binary.BigEndian.AppendUint32(nil, 2)
				a = chunk(a, "acTL", binary.BigEndian.AppendUint32(actl, 0))
				a = chunk(a, "fcTL", fc)
			case typ == "IDAT" && i == 0:
				a = chunk(a, "IDAT", data)
			case typ == "IDAT":
				if fc != nil {
					a = chunk(a, "fcTL", fc)
					fc = nil
				}
				a = chunk(a, "fdAT", append(binary.BigEndian.AppendUint32(nil, seq), data...))
				seq++
			}
			p += 12 + n
		}
	}
	a = chunk(a, "IEND", nil)
	// A WebP: a 4×4 canvas, frames of gray 10 (all of it) and 20 (2×2 at 2,
	// 2, not blended, disposed to the background), looping twice.
	var w []byte
	w = append(w, "WEBPVP8X"...)
	w = binary.LittleEndian.AppendUint32(w, 10)
	w = append(w, 0x12, 0, 0, 0, 3, 0, 0, 3, 0, 0)
	w = append(w, "ANIM"...)
	w = binary.LittleEndian.AppendUint32(w, 6)
	w = append(w, 0, 0, 0, 0, 2, 0)
	for _, f := range []struct{ x, y, w, h, c, flags int }{{0, 0, 4, 4, 10, 0}, {2, 2, 2, 2, 20, 3}} {
		var fr []byte
		put24 := func(v int) { fr = append(fr, byte(v), byte(v>>8), byte(v>>16)) }
		put24(f.x / 2)
		put24(f.y / 2)
		put24(f.w - 1)
		put24(f.h - 1)
		put24(40)
		fr = append(fr, byte(f.flags))
		fr = append(fr, "VP8L"...)
		fr = binary.LittleEndian.AppendUint32(fr, 1)
		fr = append(fr, byte(f.c), 0)
		w = append(w, "ANMF"...)
		w = binary.LittleEndian.AppendUint32(w, uint32(len(fr)))
		w = append(w, fr...)
	}
	webp := append(append([]byte("RIFF"), binary.LittleEndian.AppendUint32(nil, uint32(len(w)))...), w...)
	for name, b := range map[string][]byte{"anim.gif": g.Bytes(), "anim.png": a, "anim.webp": webp} {
		if err := os.WriteFile(filepath.Join(dir, name), b, 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

// TestImageCache runs the images' cache's C (src/imagecache.c) on a
// directory of its own: what's written reads back, names shared by two
// URLs miss, the least recently used go past the limit.
func TestImageCache(t *testing.T) {
	cc, err := cbuild.Find()
	if err != nil {
		t.Skip(err)
	}
	dir := t.TempDir()
	exe := filepath.Join(dir, "imagecache_test")
	args := append(append([]string{}, cc.Args...), cbuild.Flags...)
	args = append(args, "-Iinclude", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "src/imagecache.c", "src/files.c", "test/imagecache_test.c", "-o", exe)
	if out, err := exec.Command(cc.Path, args...).CombinedOutput(); err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	out, err := exec.Command(exe, filepath.Join(dir, "cache")).CombinedOutput()
	if err != nil || !strings.Contains(string(out), "ok") {
		t.Fatalf("%v\n%s", err, out)
	}
}

// TestAnim decodes GIFs with the C (src/animimage.c) and plays them, frame by
// frame, as internal/animimage does in Go: their canvases the same.
func TestAnim(t *testing.T) {
	cc, err := cbuild.Find()
	if err != nil {
		t.Skip(err)
	}
	dir := t.TempDir()
	exe := filepath.Join(dir, "anim_test")
	args := append(append([]string{}, cc.Args...), cbuild.Flags...)
	args = append(args, "-Iinclude", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "src/animimage.c", "src/files.c", "src/jsontree.c", "test/anim_test.c", "-o", exe)
	if out, err := exec.Command(cc.Path, args...).CombinedOutput(); err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	red, blue := color.NRGBA{255, 0, 0, 255}, color.NRGBA{0, 0, 255, 255}
	pal := color.Palette{color.NRGBA{}, red, blue, color.NRGBA{0, 255, 0, 255}}
	paletted := func(r image.Rectangle, c uint8) *image.Paletted {
		p := image.NewPaletted(r, pal)
		for i := range p.Pix {
			p.Pix[i] = c
		}
		return p
	}
	gifs := map[string]*gif.GIF{
		"disposal": {Image: []*image.Paletted{paletted(image.Rect(0, 0, 4, 4), 1), paletted(image.Rect(2, 2, 4, 4), 2)},
			Delay: []int{5, 0}, Disposal: []byte{0, gif.DisposalBackground}, LoopCount: 2, Config: image.Config{Width: 4, Height: 4}},
		"previous": {Image: []*image.Paletted{paletted(image.Rect(0, 0, 6, 5), 1), paletted(image.Rect(1, 1, 4, 3), 3), paletted(image.Rect(0, 0, 2, 2), 2)},
			Delay: []int{20, 30, 40}, Disposal: []byte{0, gif.DisposalPrevious, 0}, Config: image.Config{Width: 6, Height: 5}},
		"transparent": {Image: []*image.Paletted{paletted(image.Rect(0, 0, 3, 3), 2), paletted(image.Rect(0, 0, 3, 3), 0)},
			Delay: []int{10, 10}, LoopCount: -1, Config: image.Config{Width: 3, Height: 3}},
	}
	// A bigger one: LZW codes past 9 bits.
	big := image.NewPaletted(image.Rect(0, 0, 61, 47), pal)
	for i := range big.Pix {
		big.Pix[i] = uint8((i*7 + i/13) % 4)
	}
	gifs["big"] = &gif.GIF{Image: []*image.Paletted{big, paletted(image.Rect(5, 5, 20, 30), 3)}, Delay: []int{3, 7}, Config: image.Config{Width: 61, Height: 47}}
	for name, g := range gifs {
		var b bytes.Buffer
		if err := gif.EncodeAll(&b, g); err != nil {
			t.Fatal(err)
		}
		path := filepath.Join(dir, name+".gif")
		if err := os.WriteFile(path, b.Bytes(), 0o644); err != nil {
			t.Fatal(err)
		}
		out, err := exec.Command(exe, path).CombinedOutput()
		if err != nil {
			t.Fatalf("%s: %v\n%s", name, err, out)
		}
		a, err := animimage.Decode(b.Bytes(), nil)
		if err != nil {
			t.Fatal(err)
		}
		var want strings.Builder
		fmt.Fprintf(&want, "animated %d\n%dx%d loops %d frames %d\n", map[bool]int{false: 0, true: 1}[animimage.IsAnimated(b.Bytes())], a.Width, a.Height, a.Loops, len(a.Frames))
		p := animimage.NewPlayer(a)
		for range 8 {
			d, done := p.Next()
			if done {
				want.WriteString("-1 ")
				break
			}
			fmt.Fprintf(&want, "%d %x\n", d.Milliseconds(), p.Canvas().Pix)
		}
		want.WriteString("\n")
		if string(out) != want.String() {
			t.Errorf("%s:\n got %s\nwant %s", name, out, want.String())
		}
	}
}
