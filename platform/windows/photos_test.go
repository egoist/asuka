package windows_test

import (
	"bytes"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// platform/photos on Windows (testdata/photos.asuka): the library is the
// user's Pictures and Videos folders. A photo saved into an album made by
// createAlbum (a folder of Pictures') is listed with its size, counted,
// thumbnailed and exported; copied into another album (addToAlbum) and
// deleted from it to the Recycle Bin; the change heard (nextChange); a
// video saved to Videos is listed as one. What the app made is removed.
func TestPhotoLibrary(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	img := image.NewRGBA(image.Rect(0, 0, 40, 30))
	for y := 0; y < 30; y++ {
		for x := 0; x < 40; x++ {
			img.Set(x, y, color.RGBA{uint8(x * 6), uint8(y * 8), 128, 255})
		}
	}
	var b bytes.Buffer
	if err := png.Encode(&b, img); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(local, "a.png"), b.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
	// A movie's boxes, enough for a file of its kind (Windows' index may
	// read nothing of it).
	if err := os.WriteFile(filepath.Join(local, "v.mp4"), []byte("\x00\x00\x00\x18ftypisom\x00\x00\x02\x00isommp41\x00\x00\x00\x08free"), 0o644); err != nil {
		t.Fatal(err)
	}
	src, err := os.ReadFile(filepath.Join("testdata", "photos.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	esc := func(s string) string { return strings.ReplaceAll(s, `\`, `\\`) }
	app := strings.NewReplacer("PHOTOS_PNG", esc(guest+`\a.png`), "PHOTOS_MP4", esc(guest+`\v.mp4`)).Replace(string(src))
	buildApp(t, w, app, local)
	d := script(t, w, local, guest, "app.exe", "wait:6s", "snap:done")
	dump := d["done"]
	t.Logf("%s", dump)
	shows(t, dump, "the library", []string{
		"access: allowed",
		"album: true 0 false",
		"listed: 1 true 40x30 false true",
		"thumbnail: true",
		"export: a.png true",
		"kind: 0",
		"albums: 1 1",
		"copied: 1 0",
		"deleted: true 0",
		"noalbum: photos: no such album",
		"video: 1 true",
		"counted: true",
		"heard: AsukaPhotoTest/a.png",
		"step: done",
	})
}
