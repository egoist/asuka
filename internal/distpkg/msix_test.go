package distpkg

import (
	"archive/zip"
	"bytes"
	"compress/flate"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/xml"
	"io"
	"io/fs"
	"math/rand"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// An MSIX read back as Windows reads it: its parts by their OPC names,
// [Content_Types].xml's types for every file, and the block map's sizes,
// local header sizes and each 64 KB block's hash, its compressed bytes
// inflating on their own to the block; the ZIP's end of central
// directory ZIP64's (makeappx's: what signers keep).
func TestMSIX(t *testing.T) {
	rnd := rand.New(rand.NewSource(1))
	big := make([]byte, 3*msixBlock+1234) // four blocks, incompressible
	rnd.Read(big)
	exact := bytes.Repeat([]byte("asuka "), msixBlock/6+1)[:msixBlock] // one whole block
	files := []File{
		{Path: "My App.exe", Data: big},
		{Path: "asuka-app.json", Data: []byte(`{"name":"My App"}`)},
		{Path: "assets/exact.txt", Data: exact},
		{Path: "assets/empty", Data: nil},
		{Path: "assets/dir", Mode: fs.ModeDir | 0o755}, // a directory: left out
	}
	manifest := []byte(`<?xml version="1.0" encoding="utf-8"?><Package/>`)
	out := filepath.Join(t.TempDir(), "app.msix")
	if err := WriteMSIX(&MSIX{Manifest: manifest, Files: files}, out); err != nil {
		t.Fatal(err)
	}
	raw, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	z, err := zip.NewReader(bytes.NewReader(raw), int64(len(raw)))
	if err != nil {
		t.Fatal(err)
	}
	var names []string
	parts := map[string]*zip.File{}
	for _, f := range z.File {
		names = append(names, f.Name)
		parts[f.Name] = f
		if f.Flags&0x8 != 0 {
			t.Errorf("%s has a data descriptor", f.Name)
		}
	}
	want := []string{"AppxManifest.xml", "My%20App.exe", "asuka-app.json", "assets/exact.txt", "assets/empty", "AppxBlockMap.xml", "[Content_Types].xml"}
	if strings.Join(names, " ") != strings.Join(want, " ") {
		t.Fatalf("parts %v, want %v", names, want)
	}
	read := func(name string) []byte {
		r, err := parts[name].Open()
		if err != nil {
			t.Fatal(err)
		}
		defer r.Close()
		b, err := io.ReadAll(r)
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		return b
	}
	if !bytes.Equal(read("My%20App.exe"), big) || !bytes.Equal(read("assets/exact.txt"), exact) || !bytes.Equal(read("AppxManifest.xml"), manifest) {
		t.Error("a file isn't what was written")
	}

	var types struct {
		Defaults []struct {
			Extension   string `xml:"Extension,attr"`
			ContentType string `xml:"ContentType,attr"`
		} `xml:"Default"`
		Overrides []struct {
			PartName    string `xml:"PartName,attr"`
			ContentType string `xml:"ContentType,attr"`
		} `xml:"Override"`
	}
	if err := xml.Unmarshal(read("[Content_Types].xml"), &types); err != nil {
		t.Fatal(err)
	}
	typed := map[string]string{}
	for _, d := range types.Defaults {
		typed["."+d.Extension] = d.ContentType
	}
	for _, o := range types.Overrides {
		typed[o.PartName] = o.ContentType
	}
	for k, v := range map[string]string{".exe": "application/x-msdownload", ".json": "application/json", ".txt": "text/plain",
		"/assets/empty": "application/octet-stream", "/AppxManifest.xml": "application/vnd.ms-appx.manifest+xml",
		"/AppxBlockMap.xml": "application/vnd.ms-appx.blockmap+xml"} {
		if typed[k] != v {
			t.Errorf("%s's type is %q, want %q", k, typed[k], v)
		}
	}

	var bm struct {
		HashMethod string `xml:"HashMethod,attr"`
		Files      []struct {
			Name    string `xml:"Name,attr"`
			Size    int64  `xml:"Size,attr"`
			LfhSize int    `xml:"LfhSize,attr"`
			Blocks  []struct {
				Hash string `xml:"Hash,attr"`
				Size *int   `xml:"Size,attr"`
			} `xml:"Block"`
		} `xml:"File"`
	}
	if err := xml.Unmarshal(read("AppxBlockMap.xml"), &bm); err != nil {
		t.Fatal(err)
	}
	contents := map[string][]byte{`AppxManifest.xml`: manifest, `My App.exe`: big, `asuka-app.json`: files[1].Data, `assets\exact.txt`: exact, `assets\empty`: nil}
	if len(bm.Files) != len(contents) {
		t.Fatalf("the block map has %d files, want %d", len(bm.Files), len(contents))
	}
	for _, f := range bm.Files {
		data, ok := contents[f.Name]
		if !ok {
			t.Errorf("the block map names %q", f.Name)
			continue
		}
		zf := parts[partName(strings.ReplaceAll(f.Name, `\`, "/"))]
		off, _ := zf.DataOffset()
		lfh := int64(30 + len(zf.Name))
		if f.Size != int64(len(data)) || int64(f.LfhSize) != lfh {
			t.Errorf("%s: size %d, header %d; want %d, %d", f.Name, f.Size, f.LfhSize, len(data), lfh)
		}
		if want := (len(data) + msixBlock - 1) / msixBlock; len(f.Blocks) != want {
			t.Errorf("%s: %d blocks, want %d", f.Name, len(f.Blocks), want)
			continue
		}
		at := off
		for i, b := range f.Blocks {
			chunk := data[i*msixBlock : min(len(data), (i+1)*msixBlock)]
			sum := sha256.Sum256(chunk)
			if b.Hash != base64.StdEncoding.EncodeToString(sum[:]) {
				t.Errorf("%s: block %d's hash", f.Name, i)
			}
			if b.Size == nil {
				t.Errorf("%s: block %d has no compressed size", f.Name, i)
				continue
			}
			// The block's compressed bytes, on their own.
			got, _ := io.ReadAll(flate.NewReader(bytes.NewReader(raw[at : at+int64(*b.Size)])))
			if !bytes.Equal(got, chunk) {
				t.Errorf("%s: block %d doesn't inflate on its own (%d bytes of %d)", f.Name, i, len(got), len(chunk))
			}
			at += int64(*b.Size)
		}
		if len(f.Blocks) > 0 && at != off+int64(zf.CompressedSize64) {
			t.Errorf("%s: its blocks are %d bytes, its data %d", f.Name, at-off, zf.CompressedSize64)
		}
	}

	// The end of the central directory: ZIP64's record and locator, the
	// classic one's fields all ones.
	end := raw[len(raw)-22:]
	if binary.LittleEndian.Uint32(end) != 0x06054b50 || binary.LittleEndian.Uint32(end[16:]) != 0xFFFFFFFF || binary.LittleEndian.Uint16(end[10:]) != 0xFFFF {
		t.Errorf("the end of the central directory: % x", end)
	}
	if loc := raw[len(raw)-42:]; binary.LittleEndian.Uint32(loc) != 0x07064b50 {
		t.Errorf("no ZIP64 locator: % x", loc[:20])
	}

	// Names the package keeps for itself, twice in a case-insensitive
	// file system.
	for _, bad := range [][]File{{{Path: "AppxBlockMap.xml"}}, {{Path: "a.txt"}, {Path: "A.TXT"}}} {
		if err := WriteMSIX(&MSIX{Manifest: manifest, Files: bad}, out); err == nil {
			t.Errorf("%v: no error", bad)
		}
	}
}
