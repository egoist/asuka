package qrcode

import (
	"fmt"
	"image"
	"image/color"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/barcode"
)

// img draws a code as a reader sees it: 4 pixels a module, a quiet zone
// of four.
func img(c *Code) image.Image {
	const s, q = 4, 4
	n := (c.Size + 2*q) * s
	im := image.NewGray(image.Rect(0, 0, n, n))
	for y := 0; y < n; y++ {
		for x := 0; x < n; x++ {
			mx, my := x/s-q, y/s-q
			v := uint8(255)
			if mx >= 0 && my >= 0 && mx < c.Size && my < c.Size && c.Dark(mx, my) {
				v = 0
			}
			im.SetGray(x, y, color.Gray{v})
		}
	}
	return im
}

// Codes of every mode, at every level, from version 1 to 40, read back by
// Asuka's own reader.
func TestReadBack(t *testing.T) {
	texts := []string{
		"",
		"0",
		"01234567",
		"HELLO WORLD",
		"https://example.com/a?b=c",
		"Hello, 世界! ✓",
		strings.Repeat("1234567890", 70),
		strings.Repeat("ASUKA $%*+-./: ", 40),
		strings.Repeat("The quick brown fox jumps over the lazy dog. ", 25),
	}
	versions := map[int]bool{}
	for _, text := range texts {
		for l := Low; l <= High; l++ {
			c, err := Encode(text, l)
			if err == ErrTooLong {
				continue
			}
			if err != nil {
				t.Fatal(err)
			}
			versions[c.Version] = true
			if c.Size != c.Version*4+17 {
				t.Fatalf("size %d for version %d", c.Size, c.Version)
			}
			got := barcode.Decode(img(c), barcode.QR)
			if len(got) != 1 || got[0].Text != text {
				t.Errorf("%q at level %d (version %d, mask %d): read %v", short(text), l, c.Version, c.Mask, got)
			}
		}
	}
	// Each version, by a text that just fits it at Low (bytes mode).
	for v := 1; v <= 40; v++ {
		n := (dataCodewords(v, Low)*8 - 4 - countBits(modeBytes, v)) / 8
		text := strings.Repeat("a", n)
		c, err := Encode(text, Low)
		if err != nil || c.Version != v {
			t.Fatalf("version %d: %v, got version %v", v, err, c)
		}
		if got := barcode.Decode(img(c), barcode.QR); len(got) != 1 || got[0].Text != text {
			t.Errorf("version %d (mask %d): read %d codes", v, c.Mask, len(got))
		}
		if _, err := Encode(text+"a", Low); err == nil {
			if c2, _ := Encode(text+"a", Low); c2.Version != v+1 {
				t.Errorf("one more byte than version %d holds: version %d", v, c2.Version)
			}
		}
	}
	if _, err := Encode(strings.Repeat("a", 3000), Low); err != ErrTooLong {
		t.Errorf("3000 bytes: %v", err)
	}
}

func short(s string) string {
	if len(s) > 20 {
		return fmt.Sprintf("%s… (%d bytes)", s[:20], len(s))
	}
	return s
}

// Mode picks: the fewest bits.
func TestModes(t *testing.T) {
	for _, c := range []struct {
		text string
		mode int
	}{{"123", modeNumeric}, {"ABC 1", modeAlnum}, {"abc", modeBytes}, {"", modeBytes}} {
		if m, _ := segment(c.text); m != c.mode {
			t.Errorf("%q: mode %d, want %d", c.text, m, c.mode)
		}
	}
	// The standard's example: "01234567" at M is version 1.
	if c, _ := Encode("01234567", Medium); c.Version != 1 {
		t.Errorf("version %d", c.Version)
	}
}

func TestTerminal(t *testing.T) {
	c, _ := Encode("hi", Low)
	out := c.Terminal()
	lines := strings.Split(strings.TrimSuffix(out, "\n"), "\n")
	if len(lines) != (c.Size+4+1)/2 {
		t.Errorf("%d lines for size %d", len(lines), c.Size)
	}
}
