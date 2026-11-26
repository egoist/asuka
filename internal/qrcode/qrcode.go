// Package qrcode makes QR codes (ISO/IEC 18004): a text's smallest
// version at an error correction level, in the numeric, alphanumeric or
// byte mode (UTF-8), its mask the one whose penalty is lowest. The CLI
// prints one in a terminal (asuka dev --lan); std/qrcode is the same
// encoder in Asuka, for apps, and its tests check it against this one.
package qrcode

import (
	"errors"
	"strings"
)

// Level is an error correction level: how much of the code can be lost
// and still read.
type Level int

const (
	Low      Level = iota // about 7%
	Medium                // about 15%
	Quartile              // about 25%
	High                  // about 30%
)

// ErrTooLong is a text no version holds at the level.
var ErrTooLong = errors.New("qrcode: the text is too long for a QR code at this level")

// Code is a QR code: Size modules a side, row by row.
type Code struct {
	Size    int
	Version int
	Mask    int
	modules []bool
}

// Dark reports whether the module at x, y is dark.
func (c *Code) Dark(x, y int) bool { return c.modules[y*c.Size+x] }

// Encode makes text's QR code at the level: the smallest version that
// holds it.
func Encode(text string, level Level) (*Code, error) {
	mode, data := segment(text)
	for v := 1; v <= 40; v++ {
		capacity := dataCodewords(v, level) * 8
		used := 4 + countBits(mode, v) + len(data)
		if used <= capacity {
			return build(v, level, mode, text, data, capacity), nil
		}
	}
	return nil, ErrTooLong
}

const (
	modeNumeric = 1
	modeAlnum   = 2
	modeBytes   = 4
)

const alnum = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"

// segment picks the mode that holds text in the fewest bits and gives
// its data's bits.
func segment(text string) (int, []bool) {
	var b bits
	switch {
	case text != "" && strings.Trim(text, "0123456789") == "":
		for i := 0; i < len(text); i += 3 {
			n := len(text) - i
			if n > 3 {
				n = 3
			}
			v := 0
			for _, c := range text[i : i+n] {
				v = v*10 + int(c-'0')
			}
			b.put(v, n*3+1)
		}
		return modeNumeric, b
	case text != "" && strings.Trim(text, alnum) == "":
		for i := 0; i+1 < len(text); i += 2 {
			b.put(strings.IndexByte(alnum, text[i])*45+strings.IndexByte(alnum, text[i+1]), 11)
		}
		if len(text)%2 == 1 {
			b.put(strings.IndexByte(alnum, text[len(text)-1]), 6)
		}
		return modeAlnum, b
	}
	for i := 0; i < len(text); i++ {
		b.put(int(text[i]), 8)
	}
	return modeBytes, b
}

// countBits is the width of a segment's character count.
func countBits(mode, version int) int {
	i := 0
	if version >= 27 {
		i = 2
	} else if version >= 10 {
		i = 1
	}
	switch mode {
	case modeNumeric:
		return [3]int{10, 12, 14}[i]
	case modeAlnum:
		return [3]int{9, 11, 13}[i]
	}
	return [3]int{8, 16, 16}[i]
}

type bits []bool

func (b *bits) put(v, n int) {
	for i := n - 1; i >= 0; i-- {
		*b = append(*b, v>>i&1 == 1)
	}
}

// The codewords of error correction in each block, and the blocks, by
// level and version (index 0 unused).
var eccPerBlock = [4][41]int{
	{-1, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
	{-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},
	{-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
	{-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
}

var blocks = [4][41]int{
	{-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6, 6, 6, 6, 7, 8, 8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},
	{-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},
	{-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8, 8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},
	{-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81},
}

// rawModules is how many modules of a version hold data and error
// correction: all but the function patterns and format and version
// information.
func rawModules(v int) int {
	n := (16*v+128)*v + 64
	if v >= 2 {
		a := v/7 + 2
		n -= (25*a-10)*a - 55
		if v >= 7 {
			n -= 36
		}
	}
	return n
}

func dataCodewords(v int, l Level) int {
	return rawModules(v)/8 - eccPerBlock[l][v]*blocks[l][v]
}

// build lays out the code of a version holding the segment.
func build(v int, l Level, mode int, text string, data []bool, capacity int) *Code {
	var b bits
	b.put(mode, 4)
	b.put(len(text), countBits(mode, v)) // characters, or bytes: one each in these modes
	b = append(b, data...)
	// The terminator, up to four zeros; then to a byte; then the pad
	// bytes, in turn.
	t := capacity - len(b)
	if t > 4 {
		t = 4
	}
	b.put(0, t)
	b.put(0, (8-len(b)%8)%8)
	for pad := 0xec; len(b) < capacity; pad ^= 0xec ^ 0x11 {
		b.put(pad, 8)
	}
	words := make([]int, len(b)/8)
	for i, on := range b {
		if on {
			words[i>>3] |= 1 << (7 - i&7)
		}
	}
	all := interleave(words, v, l)

	c := &Code{Size: v*4 + 17, Version: v}
	g := newGrid(c.Size)
	g.functionPatterns(v)
	g.place(all)
	// The mask whose penalty is lowest; the first of equals.
	best, bestPenalty := 0, -1
	for m := 0; m < 8; m++ {
		g.mask(m)
		g.format(l, m)
		if p := g.penalty(); bestPenalty < 0 || p < bestPenalty {
			best, bestPenalty = m, p
		}
		g.mask(m) // undone: it's its own inverse
	}
	g.mask(best)
	g.format(l, best)
	c.Mask = best
	c.modules = g.dark
	return c
}

// interleave splits the data into the version's blocks, adds each one's
// error correction, and takes the codewords a column at a time.
func interleave(data []int, v int, l Level) []int {
	nb := blocks[l][v]
	ecc := eccPerBlock[l][v]
	raw := rawModules(v) / 8
	short := nb - raw%nb
	shortLen := raw / nb
	div := rsDivisor(ecc)
	var bs [][]int
	k := 0
	for i := 0; i < nb; i++ {
		n := shortLen - ecc
		if i >= short {
			n++
		}
		d := data[k : k+n]
		k += n
		block := append(append([]int{}, d...), rsRemainder(d, div)...)
		if i < short {
			// A short block gets a gap where the long ones have their
			// last data codeword, so the columns line up.
			block = append(block[:n:n], append([]int{-1}, block[n:]...)...)
		}
		bs = append(bs, block)
	}
	var out []int
	for i := range bs[0] {
		for _, block := range bs {
			if block[i] >= 0 {
				out = append(out, block[i])
			}
		}
	}
	return out
}

// mul multiplies in GF(2^8) modulo x^8 + x^4 + x^3 + x^2 + 1.
func mul(x, y int) int {
	z := 0
	for i := 7; i >= 0; i-- {
		z = z<<1 ^ (z>>7)*0x11d
		z ^= (y >> i & 1) * x
	}
	return z
}

// rsDivisor is the generator polynomial of degree n, highest term first
// and left out.
func rsDivisor(n int) []int {
	r := make([]int, n)
	r[n-1] = 1
	root := 1
	for i := 0; i < n; i++ {
		for j := range r {
			r[j] = mul(r[j], root)
			if j+1 < n {
				r[j] ^= r[j+1]
			}
		}
		root = mul(root, 2)
	}
	return r
}

func rsRemainder(data, div []int) []int {
	r := make([]int, len(div))
	for _, b := range data {
		f := b ^ r[0]
		copy(r, r[1:])
		r[len(r)-1] = 0
		for i, d := range div {
			r[i] ^= mul(d, f)
		}
	}
	return r
}

type grid struct {
	size  int
	dark  []bool
	fixed []bool // a function pattern's: not data, not masked
}

func newGrid(size int) *grid {
	return &grid{size: size, dark: make([]bool, size*size), fixed: make([]bool, size*size)}
}

func (g *grid) set(x, y int, on bool) {
	g.dark[y*g.size+x] = on
	g.fixed[y*g.size+x] = true
}

func (g *grid) functionPatterns(v int) {
	n := g.size
	for i := 0; i < n; i++ {
		g.set(6, i, i%2 == 0)
		g.set(i, 6, i%2 == 0)
	}
	g.finder(3, 3)
	g.finder(n-4, 3)
	g.finder(3, n-4)
	pos := alignments(v)
	for i, a := range pos {
		for j, b := range pos {
			if i == 0 && j == 0 || i == 0 && j == len(pos)-1 || i == len(pos)-1 && j == 0 {
				continue // the finders'
			}
			for dy := -2; dy <= 2; dy++ {
				for dx := -2; dx <= 2; dx++ {
					g.set(a+dx, b+dy, max(abs(dx), abs(dy)) != 1)
				}
			}
		}
	}
	g.format(Low, 0) // reserved; written for real once the mask is chosen
	if v >= 7 {
		r := v
		for i := 0; i < 12; i++ {
			r = r<<1 ^ (r>>11)*0x1f25
		}
		bitsv := v<<12 | r
		for i := 0; i < 18; i++ {
			on := bitsv>>i&1 == 1
			a, b := n-11+i%3, i/3
			g.set(a, b, on)
			g.set(b, a, on)
		}
	}
}

func (g *grid) finder(cx, cy int) {
	for dy := -4; dy <= 4; dy++ {
		for dx := -4; dx <= 4; dx++ {
			x, y := cx+dx, cy+dy
			if x >= 0 && x < g.size && y >= 0 && y < g.size {
				d := max(abs(dx), abs(dy))
				g.set(x, y, d != 2 && d != 4)
			}
		}
	}
}

// alignments are the alignment patterns' centers on each axis.
func alignments(v int) []int {
	if v == 1 {
		return nil
	}
	count := v/7 + 2
	step := (v*8 + count*3 + 5) / (count*4 - 4) * 2
	out := []int{6}
	for p := v*4 + 17 - 7; len(out) < count; p -= step {
		out = append(out[:1], append([]int{p}, out[1:]...)...)
	}
	return out
}

// format writes the level's and mask's format information, both copies,
// and the dark module.
func (g *grid) format(l Level, mask int) {
	levelBits := [4]int{1, 0, 3, 2}[l]
	d := levelBits<<3 | mask
	r := d
	for i := 0; i < 10; i++ {
		r = r<<1 ^ (r>>9)*0x537
	}
	b := (d<<10 | r) ^ 0x5412
	n := g.size
	bit := func(i int) bool { return b>>i&1 == 1 }
	for i := 0; i <= 5; i++ {
		g.set(8, i, bit(i))
	}
	g.set(8, 7, bit(6))
	g.set(8, 8, bit(7))
	g.set(7, 8, bit(8))
	for i := 9; i < 15; i++ {
		g.set(14-i, 8, bit(i))
	}
	for i := 0; i < 8; i++ {
		g.set(n-1-i, 8, bit(i))
	}
	for i := 8; i < 15; i++ {
		g.set(8, n-15+i, bit(i))
	}
	g.set(8, n-8, true)
}

// place lays the codewords in the zigzag, two columns at a time from the
// right, skipping the timing column and the function patterns.
func (g *grid) place(words []int) {
	n := g.size
	i := 0
	for right := n - 1; right >= 1; right -= 2 {
		if right == 6 {
			right = 5
		}
		for vert := 0; vert < n; vert++ {
			for j := 0; j < 2; j++ {
				x := right - j
				up := (right+1)&2 == 0
				y := vert
				if up {
					y = n - 1 - vert
				}
				if !g.fixed[y*n+x] && i < len(words)*8 {
					g.dark[y*n+x] = words[i>>3]>>(7-i&7)&1 == 1
					i++
				}
			}
		}
	}
}

func (g *grid) mask(m int) {
	n := g.size
	for y := 0; y < n; y++ {
		for x := 0; x < n; x++ {
			if g.fixed[y*n+x] {
				continue
			}
			var flip bool
			switch m {
			case 0:
				flip = (x+y)%2 == 0
			case 1:
				flip = y%2 == 0
			case 2:
				flip = x%3 == 0
			case 3:
				flip = (x+y)%3 == 0
			case 4:
				flip = (x/3+y/2)%2 == 0
			case 5:
				flip = x*y%2+x*y%3 == 0
			case 6:
				flip = (x*y%2+x*y%3)%2 == 0
			case 7:
				flip = ((x+y)%2+x*y%3)%2 == 0
			}
			if flip {
				g.dark[y*n+x] = !g.dark[y*n+x]
			}
		}
	}
}

// penalty scores a masked code as the standard does: runs of five or
// more, 2×2 blocks, finder-like patterns, and dark modules far from half.
func (g *grid) penalty() int {
	n := g.size
	p := 0
	at := func(x, y int, rows bool) bool {
		if rows {
			return g.dark[y*n+x]
		}
		return g.dark[x*n+y]
	}
	for _, rows := range []bool{true, false} {
		for y := 0; y < n; y++ {
			run := 0
			var last bool
			// The finder-like pattern: 1:1:3:1:1 with four light modules
			// on a side (the quiet zone counts as light).
			line := make([]bool, n)
			for x := 0; x < n; x++ {
				d := at(x, y, rows)
				line[x] = d
				if x > 0 && d == last {
					run++
				} else {
					if run >= 5 {
						p += run - 2
					}
					run = 1
					last = d
				}
			}
			if run >= 5 {
				p += run - 2
			}
			p += 40 * finderLike(line)
		}
	}
	for y := 0; y+1 < n; y++ {
		for x := 0; x+1 < n; x++ {
			d := g.dark[y*n+x]
			if d == g.dark[y*n+x+1] && d == g.dark[(y+1)*n+x] && d == g.dark[(y+1)*n+x+1] {
				p += 3
			}
		}
	}
	dark := 0
	for _, d := range g.dark {
		if d {
			dark++
		}
	}
	total := n * n
	// The smallest k such that the dark share is within (45 + 5k)% to
	// (55 + 5k)%.
	k := (abs(dark*20-total*10)+total-1)/total - 1
	p += k * 10
	return p
}

// finderLike counts a line's 1:1:3:1:1 patterns with four light modules
// before or after (outside the code: light).
func finderLike(line []bool) int {
	n := len(line)
	d := func(i int) bool { return i >= 0 && i < n && line[i] }
	count := 0
	for i := -4; i+11 <= n+4; i++ {
		core := d(i+4) && !d(i+5) && d(i+6) && d(i+7) && d(i+8) && !d(i+9) && d(i+10)
		if !core {
			continue
		}
		before := !d(i) && !d(i+1) && !d(i+2) && !d(i+3)
		after := !d(i+11) && !d(i+12) && !d(i+13) && !d(i+14)
		if before || after {
			count++
		}
	}
	return count
}

func abs(x int) int {
	if x < 0 {
		return -x
	}
	return x
}

// Terminal draws the code in text, two rows of modules to a line with
// half blocks, dark on light, with a quiet zone of two modules: what a
// phone's camera reads off a terminal of either theme.
func (c *Code) Terminal() string {
	const quiet = 2
	n := c.Size + 2*quiet
	dark := func(x, y int) bool {
		x, y = x-quiet, y-quiet
		return x >= 0 && y >= 0 && x < c.Size && y < c.Size && c.Dark(x, y)
	}
	var b strings.Builder
	for y := 0; y < n; y += 2 {
		// Black on white, whatever the terminal's theme: a block drawn is
		// a dark module.
		b.WriteString("\x1b[30;47m")
		for x := 0; x < n; x++ {
			top, bottom := dark(x, y), y+1 < n && dark(x, y+1)
			switch {
			case top && bottom:
				b.WriteString("█")
			case top:
				b.WriteString("▀")
			case bottom:
				b.WriteString("▄")
			default:
				b.WriteString(" ")
			}
		}
		b.WriteString("\x1b[0m\n")
	}
	return b.String()
}
