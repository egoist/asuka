package distpkg

import (
	"encoding/binary"
	"hash/adler32"
	"slices"
)

// Zlib8 is data as a zlib stream at level 8, byte for byte as the core
// compresses it (core/src/std/flate.c's ak_deflater, framed as
// compress.c's ak_cz_deflate frames zlib), data written in one piece:
// zlib's lazy matching over hash chains with Go's refinements, each block
// written with whichever of its own Huffman codes, the fixed ones or
// stored is smallest.
//
// An AppImage's SquashFS is compressed with it, so an app that updates
// its AppImage from a delta of the image's blocks decompressed (the
// update's AppImage delta) compresses the blocks it made again to the
// very bytes of the AppImage that was signed. A change here is a change
// to flate.c's level 8, and the other way round: TestZlib8MatchesCore
// compares the two.
//
// data is at most 256 KB (flate.c's window, which a write that size
// fills without sliding it): SquashFS's blocks are 128 KB at most.
func Zlib8(data []byte) []byte {
	if len(data) > fwBuf {
		panic("distpkg: Zlib8 of more than 256 KB")
	}
	d := &deflater8{win: data, out: []byte{0x78, 0xDA}}
	for i := range d.head {
		d.head[i] = fwEmpty
	}
	for i := range d.prev {
		d.prev[i] = fwEmpty
	}
	d.run(len(data))
	d.flushBlock(true)
	if d.nbits > 0 {
		d.out = append(d.out, byte(d.bits))
	}
	return binary.BigEndian.AppendUint32(d.out, adler32.Checksum(data))
}

const (
	fwSize     = 32768
	fwMask     = fwSize - 1
	fwBuf      = 8 * fwSize
	fwMinMatch = 4
	fwMaxMatch = 258
	fwLazySyms = 32768
	fwHashBits = 17
	fwEmpty    = -0x40000000
	// Level 8's search: a match this good cuts the next search's chain
	// to a quarter; one this long isn't followed by a lazy search; one
	// this long ends the search; at most this many candidates.
	fwGood, fwLazy, fwNice, fwChain = 16, 30, 40, 64
)

var (
	fwLenBase   = [29]uint16{3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258}
	fwLenExtra  = [29]uint8{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0}
	fwDistBase  = [30]uint16{1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577}
	fwDistExtra = [30]uint8{0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13}
	fwClOrder   = [19]uint8{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15}

	fwLenCode          [fwMaxMatch + 1]uint8
	fwDistLo, fwDistHi [256]uint8
)

func init() {
	for c := range 29 {
		for k := 0; k < 1<<fwLenExtra[c] && int(fwLenBase[c])+k <= fwMaxMatch; k++ {
			fwLenCode[int(fwLenBase[c])+k] = uint8(c)
		}
	}
	fwLenCode[fwMaxMatch] = 28
	for c := range 30 {
		for k := 0; k < 1<<fwDistExtra[c]; k++ {
			v := int(fwDistBase[c]) - 1 + k
			if v < 256 {
				fwDistLo[v] = uint8(c)
			} else {
				fwDistHi[v>>7] = uint8(c)
			}
		}
	}
}

func fwDistCode(dist int) int {
	if dist <= 256 {
		return int(fwDistLo[dist-1])
	}
	return int(fwDistHi[(dist-1)>>7])
}

// deflater8 is ak_deflater at level 8 over one window, win: flate.c's
// names, its state as run_lazy keeps it between calls.
type deflater8 struct {
	win        []byte
	head       [1 << fwHashBits]int32
	prev       [fwSize]int32
	blockStart int
	symEnd     int
	syms       []uint32 // a literal (< 256), or a match: dist << 9 | len
	lfreq      [288]uint32
	dfreq      [32]uint32
	out        []byte
	bits       uint64
	nbits      uint
}

func fwLoad32(b []byte, i int) uint32 { return binary.LittleEndian.Uint32(b[i:]) }

func (d *deflater8) hash(p int) uint32 {
	return (fwLoad32(d.win, p) * 2654435761) >> (32 - fwHashBits)
}

func (d *deflater8) insert(p int) int32 {
	h := d.hash(p)
	cand := d.head[h]
	d.head[h] = int32(p)
	d.prev[p&fwMask] = cand
	return cand
}

// matchLen is how many bytes from n on a and b share (the first n
// known equal), up to max.
func (d *deflater8) matchLen(a, b, n, max int) int {
	for n < max && d.win[a+n] == d.win[b+n] {
		n++
	}
	return n
}

func (d *deflater8) lit(c byte) {
	d.syms = append(d.syms, uint32(c))
	d.lfreq[c]++
	d.symEnd++
}

func (d *deflater8) match(length, dist int) {
	d.syms = append(d.syms, uint32(dist)<<9|uint32(length))
	d.lfreq[257+int(fwLenCode[length])]++
	d.dfreq[fwDistCode(dist)]++
	d.symEnd += length
}

// longest follows pos's chain from cand for a match longer than best,
// up to max: its length and distance, 0 if none is.
func (d *deflater8) longest(pos int, cand int32, best, max, chain int) (int, int) {
	w := d.win
	lo := pos - fwSize
	nice := min(fwNice, max)
	found, dist := 0, 0
	if best >= max {
		return 0, 0
	}
	first, tail := fwLoad32(w, pos), fwLoad32(w, pos+best-3)
	for int(cand) >= lo {
		m := int(cand)
		if fwLoad32(w, m+best-3) == tail && fwLoad32(w, m) == first {
			if n := d.matchLen(m, pos, 4, max); n > best {
				best, found, dist = n, n, pos-m
				if n >= nice {
					break
				}
				tail = fwLoad32(w, pos+best-3)
			}
		}
		if chain--; chain == 0 {
			break
		}
		nx := d.prev[int(cand)&fwMask]
		if nx >= cand {
			break
		}
		cand = nx
	}
	return found, dist
}

// betterAtEnd is the match at p or one starting up to 2 bytes on that
// reaches further: its length, distance and start.
func (d *deflater8) betterAtEnd(p, pos, length, dist, end int) (int, int, int) {
	w := d.win
	if length >= fwMaxMatch-2 || p+length+fwMinMatch > end {
		return length, dist, p
	}
	lim := min(end-pos, fwMaxMatch+2) + p
	lo := max(pos-fwSize, 0)
	c := int(d.head[d.hash(p+length)]) - length
	if c <= lo+2 || p-c == dist {
		return length, dist, p
	}
	if lim <= p+2 {
		return length, dist, p
	}
	n := d.matchLen(c+2, p+2, 0, lim-p-2)
	if n <= length {
		return length, dist, p
	}
	i := 2
	for i > 0 && n < fwMaxMatch && w[p+i-1] == w[c+i-1] {
		i--
		n++
	}
	return n, p - c, p + i
}

// run is run_lazy over the window to end, the input's end.
func (d *deflater8) run(end int) {
	w := d.win
	pos := 0
	prevLen, prevDist, run := 0, 0, 0
	avail := false
	for pos < end {
		if len(d.syms) >= fwLazySyms {
			d.flushBlock(false)
		}
		curLen, curDist := 0, 0
		if mx := min(end-pos, fwMaxMatch); mx >= fwMinMatch {
			cand := d.insert(pos)
			if prevLen < fwLazy && int(cand) >= pos-fwSize {
				chain := fwChain
				if prevLen >= fwGood {
					chain >>= 2
				}
				curLen, curDist = d.longest(pos, cand, max(prevLen, fwMinMatch-1), mx, max(chain, 1))
			}
		}
		if prevLen >= fwMinMatch && curLen <= prevLen {
			length, dist, start := d.betterAtEnd(pos-1, pos, prevLen, prevDist, end)
			for p := pos - 1; p < start; p++ {
				d.lit(w[p])
			}
			d.match(length, dist)
			stop := start + length
			insEnd := min(end-fwMinMatch+1, stop)
			for p := pos + 1; p < insEnd; p++ {
				d.insert(p)
			}
			pos = stop
			prevLen, run = 0, 0
			avail = false
			continue
		}
		if curLen >= fwMinMatch {
			run = 0
		}
		if !avail {
			avail = true
			prevLen, prevDist = curLen, curDist
			pos++
			continue
		}
		d.lit(w[pos-1])
		prevLen, prevDist = curLen, curDist
		pos++
		if run++; run > fwChain {
			// A long run of literals: skip ahead, searching nowhere.
			k := 1 + (run-fwChain)>>6
			for ; k > 0 && pos < end-1; k, pos = k-1, pos+1 {
				d.lit(w[pos-1])
				if pos+fwMinMatch <= end {
					d.insert(pos)
				}
			}
			d.lit(w[pos-1])
			avail = false
			prevLen = 0
		}
	}
	if avail {
		d.lit(w[pos-1])
	}
}

func (d *deflater8) put(v uint64, k uint) {
	d.bits |= v << d.nbits
	d.nbits += k
	for d.nbits >= 8 {
		d.out = append(d.out, byte(d.bits))
		d.bits >>= 8
		d.nbits -= 8
	}
}

// align ends the byte being written, its other bits zeros.
func (d *deflater8) align() {
	if d.nbits > 0 {
		d.out = append(d.out, byte(d.bits))
		d.bits, d.nbits = 0, 0
	}
}

func (d *deflater8) storedBlocks(p []byte, last bool) {
	off := 0
	for {
		k := min(len(p)-off, 65535)
		bit := uint64(0)
		if last && off+k == len(p) {
			bit = 1
		}
		d.put(bit, 3)
		d.align()
		d.out = append(d.out, byte(k), byte(k>>8), byte(^k), byte(^k>>8))
		d.out = append(d.out, p[off:off+k]...)
		off += k
		if off >= len(p) {
			return
		}
	}
}

func fwFixedLen(s int) uint8 {
	switch {
	case s < 144:
		return 8
	case s < 256:
		return 9
	case s < 280:
		return 7
	}
	return 8
}

// flushBlock writes the block gathered, [blockStart, symEnd), its
// symbols in syms, with whichever codes make it smallest.
func (d *deflater8) flushBlock(last bool) {
	lf, df := &d.lfreq, &d.dfreq
	raw := d.win[d.blockStart:d.symEnd]
	lf[256] = 1
	var ll [288]uint8
	var dl [32]uint8
	var cl [19]uint8
	var lc [288]uint16
	var dc [32]uint16
	var cc [19]uint16
	fwHuffLengths(lf[:286], 15, ll[:286])
	fwHuffLengths(df[:30], 15, dl[:30])
	hlit, hdist := 286, 30
	for hlit > 257 && ll[hlit-1] == 0 {
		hlit--
	}
	for hdist > 1 && dl[hdist-1] == 0 {
		hdist--
	}
	// The code lengths, run-length coded (3.2.7).
	all := append(append([]uint8(nil), ll[:hlit]...), dl[:hdist]...)
	var tok, tex []uint8
	var cf [19]uint32
	emit := func(t, x uint8) {
		tok = append(tok, t)
		tex = append(tex, x)
		cf[t]++
	}
	for i := 0; i < len(all); {
		l, run := all[i], 1
		for i+run < len(all) && all[i+run] == l {
			run++
		}
		i += run
		if l == 0 {
			for run >= 11 {
				k := min(run, 138)
				emit(18, uint8(k-11))
				run -= k
			}
			if run >= 3 {
				emit(17, uint8(run-3))
				run = 0
			}
		} else {
			emit(l, 0)
			run--
			for run >= 3 {
				k := min(run, 6)
				emit(16, uint8(k-3))
				run -= k
			}
		}
		for ; run > 0; run-- {
			emit(l, 0)
		}
	}
	fwHuffLengths(cf[:], 7, cl[:])
	hclen := 19
	for hclen > 4 && cl[fwClOrder[hclen-1]] == 0 {
		hclen--
	}

	// What each kind of block costs, in bits.
	var extra uint64
	dyn, fix := uint64(3+14+3*hclen), uint64(3)
	for c := range 29 {
		extra += uint64(lf[257+c]) * uint64(fwLenExtra[c])
	}
	for c := range 30 {
		extra += uint64(df[c]) * uint64(fwDistExtra[c])
	}
	for _, t := range tok {
		dyn += uint64(cl[t])
		switch t {
		case 16:
			dyn += 2
		case 17:
			dyn += 3
		case 18:
			dyn += 7
		}
	}
	for s := range 286 {
		dyn += uint64(lf[s]) * uint64(ll[s])
		fix += uint64(lf[s]) * uint64(fwFixedLen(s))
	}
	for c := range 30 {
		dyn += uint64(df[c]) * uint64(dl[c])
		fix += uint64(df[c]) * 5
	}
	dyn += extra
	fix += extra
	chunks := uint64(1)
	if len(raw) > 0 {
		chunks = uint64((len(raw) + 65534) / 65535)
	}
	stored := (chunks*5+uint64(len(raw)))*8 + 10

	if stored <= dyn && stored <= fix {
		d.storedBlocks(raw, last)
	} else {
		fixed := fix <= dyn
		if fixed {
			for s := range 288 {
				ll[s] = fwFixedLen(s)
			}
			for c := range 30 {
				dl[c] = 5
			}
		}
		fwHuffCodes(ll[:], lc[:])
		fwHuffCodes(dl[:30], dc[:30])
		lastBit := uint64(0)
		if last {
			lastBit = 1
		}
		d.put(lastBit, 1)
		if fixed {
			d.put(1, 2)
		} else {
			d.put(2, 2)
			fwHuffCodes(cl[:], cc[:])
			d.put(uint64(hlit-257), 5)
			d.put(uint64(hdist-1), 5)
			d.put(uint64(hclen-4), 4)
			for i := range hclen {
				d.put(uint64(cl[fwClOrder[i]]), 3)
			}
			for i, t := range tok {
				d.put(uint64(cc[t]), uint(cl[t]))
				switch t {
				case 16:
					d.put(uint64(tex[i]), 2)
				case 17:
					d.put(uint64(tex[i]), 3)
				case 18:
					d.put(uint64(tex[i]), 7)
				}
			}
		}
		for _, s := range d.syms {
			if s < 256 {
				d.put(uint64(lc[s]), uint(ll[s]))
				continue
			}
			length, dist := int(s&511), int(s>>9)
			c, e := int(fwLenCode[length]), fwDistCode(dist)
			d.put(uint64(lc[257+c]), uint(ll[257+c]))
			d.put(uint64(length-int(fwLenBase[c])), uint(fwLenExtra[c]))
			d.put(uint64(dc[e]), uint(dl[e]))
			d.put(uint64(dist-int(fwDistBase[e])), uint(fwDistExtra[e]))
		}
		d.put(uint64(lc[256]), uint(ll[256]))
	}
	d.lfreq, d.dfreq = [288]uint32{}, [32]uint32{}
	d.syms = d.syms[:0]
	d.blockStart = d.symEnd
}

// fwHuffLengths gives each symbol a code length from its frequency, none
// longer than limit (flate.c's huff_lengths): Moffat and Katajainen's
// in-place minimum redundancy code, its lengths then limited as miniz
// does; at least two symbols get codes.
func fwHuffLengths(freq []uint32, limit uint32, lens []uint8) {
	n := len(freq)
	key := make([]uint32, 0, n)
	for s := range n {
		lens[s] = 0
		if freq[s] != 0 {
			key = append(key, freq[s]<<9|uint32(s))
		}
	}
	for s := 0; len(key) < 2 && s < n; s++ {
		if freq[s] == 0 {
			key = append(key, 1<<9|uint32(s))
		}
	}
	m := len(key)
	if m < 2 {
		return
	}
	slices.Sort(key)
	a := make([]uint32, m)
	for i := range m {
		a[i] = key[i] >> 9
	}
	{
		root, leaf, nn := 0, 2, m
		a[0] += a[1]
		for next := 1; next < nn-1; next++ {
			if leaf >= nn || a[root] < a[leaf] {
				a[next] = a[root]
				a[root] = uint32(next)
				root++
			} else {
				a[next] = a[leaf]
				leaf++
			}
			if leaf >= nn || (root < next && a[root] < a[leaf]) {
				a[next] += a[root]
				a[root] = uint32(next)
				root++
			} else {
				a[next] += a[leaf]
				leaf++
			}
		}
		a[nn-2] = 0
		for next := nn - 3; next >= 0; next-- {
			a[next] = a[a[next]] + 1
		}
		avbl, used, dpth := 1, 0, 0
		root, next := nn-2, nn-1
		for avbl > 0 {
			for root >= 0 && int(a[root]) == dpth {
				used++
				root--
			}
			for avbl > used {
				a[next] = uint32(dpth)
				next--
				avbl--
			}
			avbl, dpth, used = 2*used, dpth+1, 0
		}
	}
	// Limit the lengths.
	var count [33]uint32
	maxlen := uint32(0)
	for i := range m {
		count[a[i]]++
		maxlen = max(maxlen, a[i])
	}
	if maxlen > limit {
		for l := limit + 1; l <= 32; l++ {
			count[limit] += count[l]
			count[l] = 0
		}
		total := uint32(0)
		for l := uint32(1); l <= limit; l++ {
			total += count[l] << (limit - l)
		}
		for total > 1<<limit {
			count[limit]--
			for l := limit - 1; l > 0; l-- {
				if count[l] != 0 {
					count[l]--
					count[l+1] += 2
					break
				}
			}
			total--
		}
		i := 0
		for l := limit; l > 0; l-- {
			for k := uint32(0); k < count[l]; k++ {
				a[i] = l
				i++
			}
		}
	}
	for i := range m {
		lens[key[i]&511] = uint8(a[i])
	}
}

// fwHuffCodes is the canonical codes of lens, bit-reversed (written LSB
// first).
func fwHuffCodes(lens []uint8, codes []uint16) {
	var count, next [16]uint32
	for _, l := range lens {
		count[l]++
	}
	count[0] = 0
	code := uint32(0)
	for l := 1; l < 16; l++ {
		code = (code + count[l-1]) << 1
		next[l] = code
	}
	for s, l := range lens {
		if l == 0 {
			codes[s] = 0
			continue
		}
		c := next[l]
		next[l]++
		r := uint32(0)
		for range l {
			r = r<<1 | c&1
			c >>= 1
		}
		codes[s] = uint16(r)
	}
}
