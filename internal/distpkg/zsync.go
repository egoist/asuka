package distpkg

import (
	"bufio"
	"crypto/sha1"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"io"
	"math"
	"time"

	"golang.org/x/crypto/md4" //nolint:staticcheck // zsync's block checksums are MD4
)

// A .zsync file (zsync 0.6.2's control file, which zsync, AppImageUpdate
// and appimageupdatetool read): what a client needs to fetch only the
// blocks of a new file it doesn't have, by range requests of the URL.
// Its header, then for each block of Blocksize bytes (the last padded
// with zeros) the trailing bytes of its rolling checksum and the leading
// bytes of its MD4, as zsyncmake writes them.

// ZsyncInfo is what a .zsync names besides the file's blocks.
type ZsyncInfo struct {
	// Filename is the file's name, as the client saves it.
	Filename string
	// URL is where it's downloaded from: absolute, or relative to the
	// .zsync's.
	URL string
	// MTime is the file's time (none written when it's zero).
	MTime time.Time
}

// zsyncBlocksize is zsyncmake's block size for a file of n bytes.
func zsyncBlocksize(n int64) int {
	if n < 100000000 {
		return 2048
	}
	return 4096
}

// zsyncHashLengths are how many consecutive blocks a client matches, and
// how many bytes of each block's rolling checksum and MD4 the file keeps,
// for a file of n bytes in blocks of bs (zsyncmake's formulas).
func zsyncHashLengths(n int64, bs int) (seq, rsum, check int) {
	seq = 1
	if n > int64(bs) {
		seq = 2
	}
	ln, lb, l2 := math.Log(float64(n)), math.Log(float64(bs)), math.Log(2)
	rsum = int(math.Ceil(((ln+lb)/l2 - 8.6) / float64(seq) / 8))
	rsum = min(max(rsum, 2), 4)
	blocks := float64(1 + n/int64(bs))
	check = int(math.Ceil((20 + (ln+math.Log(blocks))/l2) / float64(seq) / 8))
	if c2 := int((7.9 + (20 + math.Log(blocks)/l2)) / 8); check < c2 {
		check = c2
	}
	return seq, rsum, min(check, 16)
}

// zsyncRsum is a block's rolling checksum, big-endian a then b.
func zsyncRsum(block []byte) [4]byte {
	var a, b uint16
	n := len(block)
	for i, c := range block {
		a += uint16(c)
		b += uint16(n-i) * uint16(c)
	}
	var out [4]byte
	binary.BigEndian.PutUint16(out[0:], a)
	binary.BigEndian.PutUint16(out[2:], b)
	return out
}

// WriteZsync writes the .zsync of the n bytes of r to w.
func WriteZsync(w io.Writer, r io.Reader, n int64, info ZsyncInfo) error {
	if n <= 0 {
		return fmt.Errorf("distpkg: zsync: an empty file")
	}
	bs := zsyncBlocksize(n)
	seq, rsumLen, checkLen := zsyncHashLengths(n, bs)
	sum := sha1.New()
	var sums []byte
	block := make([]byte, bs)
	var got int64
	for got < n {
		k, err := io.ReadFull(r, block[:min(int64(bs), n-got)])
		if err != nil {
			return fmt.Errorf("distpkg: zsync: %w", err)
		}
		sum.Write(block[:k])
		got += int64(k)
		clear(block[k:]) // the last block, padded
		rs := zsyncRsum(block)
		m := md4.New()
		m.Write(block)
		sums = append(sums, rs[4-rsumLen:]...)
		sums = append(sums, m.Sum(nil)[:checkLen]...)
	}
	bw := bufio.NewWriter(w)
	fmt.Fprintf(bw, "zsync: 0.6.2\n")
	if info.Filename != "" {
		fmt.Fprintf(bw, "Filename: %s\n", info.Filename)
		if !info.MTime.IsZero() {
			fmt.Fprintf(bw, "MTime: %s\n", info.MTime.UTC().Format("Mon, 02 Jan 2006 15:04:05 -0700"))
		}
	}
	fmt.Fprintf(bw, "Blocksize: %d\nLength: %d\nHash-Lengths: %d,%d,%d\n", bs, n, seq, rsumLen, checkLen)
	if info.URL != "" {
		fmt.Fprintf(bw, "URL: %s\n", info.URL)
	}
	fmt.Fprintf(bw, "SHA-1: %s\n\n", hex.EncodeToString(sum.Sum(nil)))
	bw.Write(sums)
	return bw.Flush()
}
