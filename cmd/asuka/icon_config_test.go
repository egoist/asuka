package main

import (
	"encoding/json"
	"image"
	"image/color"
	"image/draw"
	"image/png"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// iconTestPNG writes a size×size PNG of c, its middle half of mid when
// mid isn't the zero color (a shape on a layer).
func iconTestPNG(t *testing.T, path string, size int, c, mid color.NRGBA) {
	t.Helper()
	img := newNRGBA(size)
	for y := 0; y < size; y++ {
		for x := 0; x < size; x++ {
			px := c
			if mid != (color.NRGBA{}) && x >= size/4 && x < size*3/4 && y >= size/4 && y < size*3/4 {
				px = mid
			}
			img.SetNRGBA(x, y, px)
		}
	}
	if err := writePNG(path, img); err != nil {
		t.Fatal(err)
	}
}

func readTestPNG(t *testing.T, path string) *image.NRGBA {
	t.Helper()
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	img, err := png.Decode(f)
	if err != nil {
		t.Fatal(err)
	}
	n := image.NewNRGBA(img.Bounds())
	draw.Draw(n, n.Bounds(), img, img.Bounds().Min, draw.Src)
	return n
}

// iconTestApp is an app whose asuka.toml is toml, with the icon PNGs
// it names made: image, foreground, background.png, monochrome, dark and
// tinted.
func iconTestApp(t *testing.T, toml string) *appConfig {
	t.Helper()
	dir := t.TempDir()
	blue := color.NRGBA{30, 60, 200, 255}
	white := color.NRGBA{255, 255, 255, 255}
	clear := color.NRGBA{0, 0, 0, 0}
	iconTestPNG(t, filepath.Join(dir, "icon.png"), 512, blue, white)
	iconTestPNG(t, filepath.Join(dir, "fg.png"), 512, clear, white)
	iconTestPNG(t, filepath.Join(dir, "bg.png"), 512, blue, color.NRGBA{})
	iconTestPNG(t, filepath.Join(dir, "mono.png"), 512, clear, white)
	iconTestPNG(t, filepath.Join(dir, "dark.png"), 512, clear, color.NRGBA{255, 160, 0, 255})
	iconTestPNG(t, filepath.Join(dir, "tinted.png"), 512, clear, color.NRGBA{200, 40, 40, 255})
	if err := os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(toml), 0o644); err != nil {
		t.Fatal(err)
	}
	c, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	return c
}

// asuka.toml's icon: a path, or a table of the image and its layers,
// their paths the app's directory's.
func TestIconConfigForms(t *testing.T) {
	c := iconTestApp(t, `icon = "icon.png"`)
	if c.Icon == "" || !filepath.IsAbs(c.Icon) || c.IconSpec.Foreground != "" {
		t.Errorf("string form: %q %+v", c.Icon, c.IconSpec)
	}
	c = iconTestApp(t, `[icon]
image = "icon.png"
foreground = "fg.png"
background = "#112233"
monochrome = "mono.png"
dark = "dark.png"
tinted = "tinted.png"
`)
	dir := filepath.Dir(c.Icon)
	want := iconConfig{Image: filepath.Join(dir, "icon.png"), Foreground: filepath.Join(dir, "fg.png"), Background: "#112233",
		Monochrome: filepath.Join(dir, "mono.png"), Dark: filepath.Join(dir, "dark.png"), Tinted: filepath.Join(dir, "tinted.png")}
	if c.IconSpec != want || c.Icon != want.Image || !c.IconSpec.backgroundColor() {
		t.Errorf("table form:\n got %+v\nwant %+v", c.IconSpec, want)
	}
	c = iconTestApp(t, "[icon]\nimage = \"icon.png\"\nforeground = \"fg.png\"\nbackground = \"bg.png\"\n")
	if c.IconSpec.backgroundColor() || c.IconSpec.Background != filepath.Join(filepath.Dir(c.Icon), "bg.png") {
		t.Errorf("background image: %+v", c.IconSpec)
	}
	for bad, says := range map[string]string{
		"[icon]\nforeground = \"fg.png\"":                                              "needs its image",
		"[icon]\nimage = \"icon.png\"\nbackground = \"#000000\"":                       "needs its foreground",
		"[icon]\nimage = \"icon.png\"\nforeground = \"fg.png\"\nbackground = \"#abc\"": "want #rrggbb",
		"icon = 3": "icon",
	} {
		dir := t.TempDir()
		os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(bad), 0o644)
		if _, err := loadConfig(dir); err == nil || !strings.Contains(err.Error(), says) {
			t.Errorf("%q: %v (want %q)", bad, err, says)
		}
	}
}

// Android's adaptive icon: asuka.toml's foreground layer, whole, over its
// background color or layer, with the themed icon's monochrome layer;
// the legacy icons still the image's.
func TestAndroidAdaptiveIconLayers(t *testing.T) {
	c := iconTestApp(t, "[icon]\nimage = \"icon.png\"\nforeground = \"fg.png\"\nbackground = \"#112233\"\nmonochrome = \"mono.png\"\n")
	res := t.TempDir()
	if err := androidIcons(c, res); err != nil {
		t.Fatal(err)
	}
	xml, err := os.ReadFile(filepath.Join(res, "mipmap-anydpi-v26", "ic_launcher.xml"))
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{
		`<background android:drawable="@color/asuka_icon_background" />`,
		`<foreground android:drawable="@mipmap/ic_launcher_foreground" />`,
		`<monochrome android:drawable="@mipmap/ic_launcher_monochrome" />`,
	} {
		if !strings.Contains(string(xml), want) {
			t.Errorf("no %s in:\n%s", want, xml)
		}
	}
	if colors, _ := os.ReadFile(filepath.Join(res, "values", "asuka_ic_launcher.xml")); !strings.Contains(string(colors), "#112233") {
		t.Errorf("background color: %s", colors)
	}
	for _, d := range androidDensities {
		dir := filepath.Join(res, "mipmap-"+d.dir)
		if n := readTestPNG(t, filepath.Join(dir, "ic_launcher.png")).Bounds().Dx(); n != int(48*d.scale) {
			t.Errorf("%s legacy: %d", d.dir, n)
		}
		for _, layer := range []string{"foreground", "monochrome"} {
			img := readTestPNG(t, filepath.Join(dir, "ic_launcher_"+layer+".png"))
			if n := img.Bounds().Dx(); n != int(108*d.scale) {
				t.Errorf("%s %s: %d", d.dir, layer, n)
			}
			// The whole layer, not inset: its corner is the layer's
			// (transparent), its middle the shape.
			if mid := img.NRGBAAt(img.Bounds().Dx()/2, img.Bounds().Dx()/2); mid.A != 255 || img.NRGBAAt(0, 0).A != 0 {
				t.Errorf("%s %s: corner %v, middle %v", d.dir, layer, img.NRGBAAt(0, 0), mid)
			}
		}
	}
	if _, err := os.Stat(filepath.Join(res, "mipmap-xxxhdpi", "ic_launcher_background.png")); err == nil {
		t.Error("a color background has a layer's PNG")
	}

	// A background layer: the PNGs, and the adaptive icon's background.
	c = iconTestApp(t, "[icon]\nimage = \"icon.png\"\nforeground = \"fg.png\"\nbackground = \"bg.png\"\n")
	res = t.TempDir()
	if err := androidIcons(c, res); err != nil {
		t.Fatal(err)
	}
	xml, _ = os.ReadFile(filepath.Join(res, "mipmap-anydpi-v26", "ic_launcher.xml"))
	if !strings.Contains(string(xml), `<background android:drawable="@mipmap/ic_launcher_background" />`) || strings.Contains(string(xml), "monochrome") {
		t.Errorf("background layer:\n%s", xml)
	}
	if n := readTestPNG(t, filepath.Join(res, "mipmap-xxhdpi", "ic_launcher_background.png")).Bounds().Dx(); n != 324 {
		t.Errorf("background layer: %d", n)
	}

	// No layers: the image inset in the middle 72dp, over its corner's
	// color, as before; a monochrome layer alone still themes it.
	c = iconTestApp(t, "[icon]\nimage = \"icon.png\"\nmonochrome = \"mono.png\"\n")
	res = t.TempDir()
	if err := androidIcons(c, res); err != nil {
		t.Fatal(err)
	}
	xml, _ = os.ReadFile(filepath.Join(res, "mipmap-anydpi-v26", "ic_launcher.xml"))
	if !strings.Contains(string(xml), "@color/asuka_icon_background") || !strings.Contains(string(xml), "ic_launcher_monochrome") {
		t.Errorf("image and monochrome:\n%s", xml)
	}
	fg := readTestPNG(t, filepath.Join(res, "mipmap-mdpi", "ic_launcher_foreground.png"))
	if fg.NRGBAAt(5, 5).A != 0 || fg.NRGBAAt(20, 20).A != 255 {
		t.Errorf("inset foreground: corner %v, inside %v", fg.NRGBAAt(5, 5), fg.NRGBAAt(20, 20))
	}
	if colors, _ := os.ReadFile(filepath.Join(res, "values", "asuka_ic_launcher.xml")); !strings.Contains(string(colors), "#1E3CC8") {
		t.Errorf("corner color: %s", colors)
	}
}

// iOS 18's dark and tinted appearances of the app's icon, in its icon
// set: the light one opaque, the dark one with its transparency, the
// tinted one grayscale.
func TestIOSIconAppearances(t *testing.T) {
	c := iconTestApp(t, "[icon]\nimage = \"icon.png\"\ndark = \"dark.png\"\ntinted = \"tinted.png\"\n")
	cat, err := writeAssetCatalog(c, t.TempDir(), true)
	if err != nil {
		t.Fatal(err)
	}
	set := filepath.Join(cat, "AppIcon.appiconset")
	data, err := os.ReadFile(filepath.Join(set, "Contents.json"))
	if err != nil {
		t.Fatal(err)
	}
	var contents struct {
		Images []struct {
			Filename    string `json:"filename"`
			Size        string `json:"size"`
			Appearances []struct {
				Appearance, Value string
			} `json:"appearances"`
		} `json:"images"`
	}
	if err := json.Unmarshal(data, &contents); err != nil {
		t.Fatal(err)
	}
	got := map[string]string{}
	for _, im := range contents.Images {
		v := ""
		for _, a := range im.Appearances {
			if a.Appearance == "luminosity" {
				v = a.Value
			}
		}
		got[v] = im.Filename
		if im.Size != "1024x1024" {
			t.Errorf("%s: size %s", im.Filename, im.Size)
		}
	}
	if got[""] != "icon.png" || got["dark"] != "icon-dark.png" || got["tinted"] != "icon-tinted.png" {
		t.Errorf("Contents.json: %s", data)
	}
	if light := readTestPNG(t, filepath.Join(set, "icon.png")); hasTransparency(light) {
		t.Error("the light icon isn't opaque")
	}
	if dark := readTestPNG(t, filepath.Join(set, "icon-dark.png")); dark.NRGBAAt(0, 0).A != 0 || dark.NRGBAAt(512, 512) != (color.NRGBA{255, 160, 0, 255}) {
		t.Errorf("dark: corner %v, middle %v", dark.NRGBAAt(0, 0), dark.NRGBAAt(512, 512))
	}
	tinted := readTestPNG(t, filepath.Join(set, "icon-tinted.png"))
	if p := tinted.NRGBAAt(512, 512); p.R != p.G || p.G != p.B || p.A != 255 || tinted.NRGBAAt(0, 0).A != 0 {
		t.Errorf("tinted: middle %v, corner %v", p, tinted.NRGBAAt(0, 0))
	}

	// Without them, the set has the icon alone, for iOS to make its own.
	c = iconTestApp(t, `icon = "icon.png"`)
	cat, err = writeAssetCatalog(c, t.TempDir(), true)
	if err != nil {
		t.Fatal(err)
	}
	data, _ = os.ReadFile(filepath.Join(cat, "AppIcon.appiconset", "Contents.json"))
	if strings.Contains(string(data), "appearances") {
		t.Errorf("plain icon: %s", data)
	}
}
