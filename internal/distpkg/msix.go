package distpkg

import (
	"bufio"
	"bytes"
	"compress/flate"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"path"
	"sort"
	"strings"
	"time"
)

// An MSIX is a Windows app package (the Microsoft Store's, App
// Installer's, Add-AppxPackage's): a ZIP of the app's files and its
// AppxManifest.xml, with AppxBlockMap.xml, every file's SHA-256 a 64 KB
// block at a time (what Windows checks the files by, and installs and
// updates them a block at a time with), and [Content_Types].xml, OPC's
// media types of its parts. Each file is DEFLATEd a block at a time, a
// block's compressed bytes on their own (as makeappx writes them: a full
// flush at each block's end), and the block map has each block's
// compressed size. It isn't signed here: signtool or osslsigncode adds
// AppxSignature.p7x, or the Store signs it.
type MSIX struct {
	// Manifest is AppxManifest.xml.
	Manifest []byte
	// Files are the package's files, their Paths relative to its root,
	// slash-separated (Notes.exe, Assets/StoreLogo.png); directories and
	// links aren't in an MSIX.
	Files []File
	// Time is the files' time in the ZIP (Epoch's by default).
	Time time.Time
}

const msixBlock = 64 << 10

// MSIXBlockMapNS and the others are the package's XML namespaces and its
// parts' media types.
const (
	MSIXBlockMapNS     = "http://schemas.microsoft.com/appx/2010/blockmap"
	msixContentTypesNS = "http://schemas.openxmlformats.org/package/2006/content-types"
	msixSHA256         = "http://www.w3.org/2001/04/xmlenc#sha256"
)

// WriteMSIX writes the package at out.
func WriteMSIX(m *MSIX, out string) error {
	if len(m.Manifest) == 0 {
		return errors.New("distpkg: an MSIX needs its AppxManifest.xml")
	}
	t := m.Time
	if t.IsZero() {
		t = Epoch()
	}
	files := []File{{Path: "AppxManifest.xml", Data: m.Manifest}}
	seen := map[string]bool{"appxmanifest.xml": true}
	for _, f := range m.Files {
		if f.IsDir() || f.IsLink() {
			continue
		}
		p := strings.TrimPrefix(path.Clean(strings.ReplaceAll(f.Path, `\`, "/")), "/")
		low := strings.ToLower(p)
		if seen[low] {
			return fmt.Errorf("distpkg: %s is in the MSIX twice (its names ignore case)", p)
		}
		if reservedMSIXName(low) {
			return fmt.Errorf("distpkg: %s is a name the MSIX keeps for itself", p)
		}
		seen[low] = true
		f.Path = p
		files = append(files, f)
	}
	tmp := out + ".tmp"
	fh, err := os.Create(tmp)
	if err != nil {
		return err
	}
	w := &msixZip{w: bufio.NewWriterSize(fh, 1<<20)}
	w.dosTime, w.dosDate = dosDateTime(t)
	var blockMap bytes.Buffer
	blockMap.WriteString(`<?xml version="1.0" encoding="UTF-8" standalone="no"?>` + "\r\n")
	fmt.Fprintf(&blockMap, `<BlockMap xmlns="%s" HashMethod="%s">`, MSIXBlockMapNS, msixSHA256)
	exts := map[string]bool{}
	var noExt []string
	for _, f := range files {
		if err := w.addFile(&f, &blockMap); err != nil {
			fh.Close()
			os.Remove(tmp)
			return err
		}
		if e := msixExt(f.Path); e != "" {
			exts[e] = true
		} else {
			noExt = append(noExt, f.Path)
		}
	}
	blockMap.WriteString("</BlockMap>")
	err = w.addData("AppxBlockMap.xml", blockMap.Bytes())
	if err == nil {
		err = w.addData("[Content_Types].xml", contentTypes(exts, noExt))
	}
	if err == nil {
		err = w.finish()
	}
	if err == nil {
		err = w.w.Flush()
	}
	if cerr := fh.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(tmp)
		return err
	}
	return os.Rename(tmp, out)
}

// reservedMSIXName reports whether a file's lower-cased path is one of
// the package's own parts.
func reservedMSIXName(p string) bool {
	switch p {
	case "appxblockmap.xml", "appxsignature.p7x", "[content_types].xml":
		return true
	}
	return strings.HasPrefix(p, "appxmetadata/")
}

func msixExt(p string) string {
	base := path.Base(p)
	if i := strings.LastIndexByte(base, '.'); i >= 0 && i < len(base)-1 {
		return strings.ToLower(base[i+1:])
	}
	return ""
}

// msixMediaTypes are the media types [Content_Types].xml gives the
// extensions it knows (makeappx's); the others are
// application/octet-stream.
var msixMediaTypes = map[string]string{
	"exe": "application/x-msdownload", "dll": "application/x-msdownload",
	"png": "image/png", "jpg": "image/jpeg", "jpeg": "image/jpeg", "gif": "image/gif", "ico": "image/vnd.microsoft.icon",
	"svg": "image/svg+xml", "webp": "image/webp", "bmp": "image/bmp",
	"xml": "application/vnd.ms-appx.manifest+xml", "json": "application/json", "txt": "text/plain",
	"html": "text/html", "htm": "text/html", "css": "text/css", "js": "application/javascript",
	"wasm": "application/wasm", "ttf": "application/x-font-ttf", "otf": "application/x-font-otf",
	"woff": "application/font-woff", "woff2": "font/woff2", "mp3": "audio/mpeg", "wav": "audio/wav",
	"mp4": "video/mp4", "pri": "application/octet-stream", "zip": "application/x-zip-compressed",
}

func contentTypes(exts map[string]bool, noExt []string) []byte {
	var b bytes.Buffer
	b.WriteString(`<?xml version="1.0" encoding="UTF-8"?>` + "\r\n")
	fmt.Fprintf(&b, `<Types xmlns="%s">`, msixContentTypesNS)
	keys := make([]string, 0, len(exts))
	for e := range exts {
		keys = append(keys, e)
	}
	sort.Strings(keys)
	for _, e := range keys {
		t := msixMediaTypes[e]
		if t == "" {
			t = "application/octet-stream"
		}
		if e == "xml" {
			t = "application/xml" // AppxManifest.xml's is an override
		}
		fmt.Fprintf(&b, `<Default Extension="%s" ContentType="%s"/>`, xmlEscape(e), t)
	}
	for _, p := range noExt {
		fmt.Fprintf(&b, `<Override PartName="/%s" ContentType="application/octet-stream"/>`, xmlEscape(partName(p)))
	}
	b.WriteString(`<Override PartName="/AppxManifest.xml" ContentType="application/vnd.ms-appx.manifest+xml"/>`)
	b.WriteString(`<Override PartName="/AppxBlockMap.xml" ContentType="application/vnd.ms-appx.blockmap+xml"/>`)
	b.WriteString("</Types>")
	return b.Bytes()
}

// partName is a file's name in the ZIP, as OPC names a part: its path's
// bytes, those that aren't a URI's unreserved or sub-delimiters
// percent-encoded (a space is %20).
func partName(p string) string {
	var b strings.Builder
	for i := 0; i < len(p); i++ {
		c := p[i]
		if c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || strings.IndexByte("-._~!$&'()+,;=@/", c) >= 0 {
			b.WriteByte(c)
		} else {
			fmt.Fprintf(&b, "%%%02X", c)
		}
	}
	return b.String()
}

func xmlEscape(s string) string {
	var b bytes.Buffer
	for _, r := range s {
		switch r {
		case '&':
			b.WriteString("&amp;")
		case '<':
			b.WriteString("&lt;")
		case '>':
			b.WriteString("&gt;")
		case '"':
			b.WriteString("&quot;")
		case '\'':
			b.WriteString("&apos;")
		default:
			b.WriteRune(r)
		}
	}
	return b.String()
}

// msixZip writes the ZIP: each file's local header and data, then the
// central directory and ZIP64's end records (always, as makeappx does),
// no data descriptors.
type msixZip struct {
	w                *bufio.Writer
	off              uint64
	dosTime, dosDate uint16
	dir              []msixEntry
}

type msixEntry struct {
	name         string
	method       uint16
	crc          uint32
	csize, usize uint64
	off          uint64
}

func (z *msixZip) write(b []byte) error {
	n, err := z.w.Write(b)
	z.off += uint64(n)
	return err
}

// addFile adds f, DEFLATEd a block at a time, and its entry in the
// block map.
func (z *msixZip) addFile(f *File, blockMap *bytes.Buffer) error {
	r, err := f.Open()
	if err != nil {
		return err
	}
	defer r.Close()
	var data bytes.Buffer
	var blocks []struct {
		hash  [32]byte
		csize int
	}
	crc := crc32.NewIEEE()
	var usize uint64
	buf := make([]byte, msixBlock)
	br := bufio.NewReaderSize(r, msixBlock)
	for {
		n, err := io.ReadFull(br, buf)
		if err != nil && err != io.EOF && err != io.ErrUnexpectedEOF {
			return err
		}
		if n == 0 {
			break
		}
		block := buf[:n]
		crc.Write(block)
		usize += uint64(n)
		// A block compressed on its own (a new compressor: no match
		// reaches back into the block before), ended byte-aligned by a
		// sync flush; the last one (nothing after it) ends the stream.
		_, perr := br.Peek(1)
		last := perr != nil
		before := data.Len()
		fw, _ := flate.NewWriter(&data, flate.DefaultCompression)
		fw.Write(block)
		if last {
			fw.Close()
		} else {
			fw.Flush()
		}
		blocks = append(blocks, struct {
			hash  [32]byte
			csize int
		}{sha256.Sum256(block), data.Len() - before})
		if last {
			break
		}
	}
	if usize >= 0xFFFFFFFF || uint64(data.Len()) >= 0xFFFFFFFF {
		return fmt.Errorf("distpkg: %s is too big for an MSIX here (4 GB or more)", f.Path)
	}
	name := partName(f.Path)
	method := uint16(8)
	payload := data.Bytes()
	if usize == 0 {
		method, payload = 0, nil
	}
	lfh := z.localHeader(name, method, crc.Sum32(), uint64(len(payload)), usize)
	fmt.Fprintf(blockMap, `<File Name="%s" Size="%d" LfhSize="%d">`, xmlEscape(strings.ReplaceAll(f.Path, "/", `\`)), usize, len(lfh))
	for _, b := range blocks {
		fmt.Fprintf(blockMap, `<Block Hash="%s" Size="%d"/>`, base64.StdEncoding.EncodeToString(b.hash[:]), b.csize)
	}
	blockMap.WriteString("</File>")
	z.dir = append(z.dir, msixEntry{name: name, method: method, crc: crc.Sum32(), csize: uint64(len(payload)), usize: usize, off: z.off})
	if err := z.write(lfh); err != nil {
		return err
	}
	return z.write(payload)
}

// addData adds one of the package's own parts, DEFLATEd whole.
func (z *msixZip) addData(name string, b []byte) error {
	var data bytes.Buffer
	fw, _ := flate.NewWriter(&data, flate.DefaultCompression)
	fw.Write(b)
	fw.Close()
	crc := crc32.ChecksumIEEE(b)
	lfh := z.localHeader(name, 8, crc, uint64(data.Len()), uint64(len(b)))
	z.dir = append(z.dir, msixEntry{name: name, method: 8, crc: crc, csize: uint64(data.Len()), usize: uint64(len(b)), off: z.off})
	if err := z.write(lfh); err != nil {
		return err
	}
	return z.write(data.Bytes())
}

func (z *msixZip) localHeader(name string, method uint16, crc uint32, csize, usize uint64) []byte {
	le := binary.LittleEndian
	h := make([]byte, 30, 30+len(name))
	le.PutUint32(h[0:], 0x04034b50)
	le.PutUint16(h[4:], 20) // version needed: 2.0
	le.PutUint16(h[6:], 0)  // flags
	le.PutUint16(h[8:], method)
	le.PutUint16(h[10:], z.dosTime)
	le.PutUint16(h[12:], z.dosDate)
	le.PutUint32(h[14:], crc)
	le.PutUint32(h[18:], uint32(csize))
	le.PutUint32(h[22:], uint32(usize))
	le.PutUint16(h[26:], uint16(len(name)))
	le.PutUint16(h[28:], 0)
	return append(h, name...)
}

// finish writes the central directory, ZIP64's end of it and its
// locator, and the end of the central directory pointing to them.
func (z *msixZip) finish() error {
	le := binary.LittleEndian
	start := z.off
	for _, e := range z.dir {
		var extra []byte
		off32 := uint32(e.off)
		if e.off >= 0xFFFFFFFF {
			off32 = 0xFFFFFFFF
			extra = make([]byte, 12)
			le.PutUint16(extra[0:], 1)
			le.PutUint16(extra[2:], 8)
			le.PutUint64(extra[4:], e.off)
		}
		h := make([]byte, 46)
		le.PutUint32(h[0:], 0x02014b50)
		le.PutUint16(h[4:], 45) // made by: 4.5 (ZIP64), MS-DOS
		version := uint16(20)
		if extra != nil {
			version = 45
		}
		le.PutUint16(h[6:], version)
		le.PutUint16(h[8:], 0)
		le.PutUint16(h[10:], e.method)
		le.PutUint16(h[12:], z.dosTime)
		le.PutUint16(h[14:], z.dosDate)
		le.PutUint32(h[16:], e.crc)
		le.PutUint32(h[20:], uint32(e.csize))
		le.PutUint32(h[24:], uint32(e.usize))
		le.PutUint16(h[28:], uint16(len(e.name)))
		le.PutUint16(h[30:], uint16(len(extra)))
		le.PutUint32(h[42:], off32)
		if err := z.write(append(append(h, e.name...), extra...)); err != nil {
			return err
		}
	}
	size := z.off - start
	at := z.off
	n := uint64(len(z.dir))
	rec := make([]byte, 56)
	le.PutUint32(rec[0:], 0x06064b50)
	le.PutUint64(rec[4:], 44)
	le.PutUint16(rec[12:], 45)
	le.PutUint16(rec[14:], 45)
	le.PutUint64(rec[24:], n)
	le.PutUint64(rec[32:], n)
	le.PutUint64(rec[40:], size)
	le.PutUint64(rec[48:], start)
	loc := make([]byte, 20)
	le.PutUint32(loc[0:], 0x07064b50)
	le.PutUint64(loc[8:], at)
	le.PutUint32(loc[16:], 1)
	end := make([]byte, 22)
	le.PutUint32(end[0:], 0x06054b50)
	// Its counts, size and offset are ZIP64's (all ones), as makeappx
	// writes them: readers go to the ZIP64 record, and signers keep it
	// (osslsigncode drops it otherwise, and Windows can't open the
	// package it signed).
	le.PutUint16(end[8:], 0xFFFF)
	le.PutUint16(end[10:], 0xFFFF)
	le.PutUint32(end[12:], 0xFFFFFFFF)
	le.PutUint32(end[16:], 0xFFFFFFFF)
	for _, b := range [][]byte{rec, loc, end} {
		if err := z.write(b); err != nil {
			return err
		}
	}
	return nil
}

func dosDateTime(t time.Time) (tm, date uint16) {
	t = t.UTC()
	if t.Year() < 1980 {
		t = time.Date(1980, 1, 1, 0, 0, 0, 0, time.UTC)
	}
	tm = uint16(t.Hour()<<11 | t.Minute()<<5 | t.Second()/2)
	date = uint16((t.Year()-1980)<<9 | int(t.Month())<<5 | t.Day())
	return
}
