package main

import (
	"fmt"
	"image"
	"image/color"
	"os"
	"path/filepath"
	"strings"
)

// androidIcons writes the launcher icons: the app's (asuka.toml's icon,
// ic_launcher, with its layers) and its alternates' (ic_launcher_<name>,
// each an activity alias's: androidManifest).
func androidIcons(cfg *appConfig, res string) error {
	ic := cfg.IconSpec
	ic.Image = cfg.Icon
	if err := androidIcon(ic, res, "ic_launcher"); err != nil {
		return err
	}
	for _, name := range cfg.alternateIconNames() {
		if err := androidIcon(iconConfig{Image: cfg.AlternateIcons[name]}, res, androidIconRes(name)); err != nil {
			return err
		}
	}
	return nil
}

// androidIconRes is an alternate icon's resource name: lower case, as
// resource names are.
func androidIconRes(name string) string { return "ic_launcher_" + strings.ToLower(name) }

// androidDensities are the launcher's densities, by their mipmap
// directories' qualifiers, and their scale of a dp.
var androidDensities = []struct {
	dir   string
	scale float64
}{{"mdpi", 1}, {"hdpi", 1.5}, {"xhdpi", 2}, {"xxhdpi", 3}, {"xxxhdpi", 4}}

// androidIcon writes a launcher icon named name: a legacy icon per
// density, from ic.Image, and an adaptive one (Android 8+). Its
// foreground is ic.Foreground's layer, else the image inside the safe
// zone; its background ic.Background's color or layer, else the image's
// corner color; and with ic.Monochrome, it has the themed icon's layer
// (Android 13+; earlier versions ignore it).
func androidIcon(ic iconConfig, res, name string) error {
	icon, err := loadIcon(ic.Image)
	if err != nil {
		return err
	}
	var fgLayer, bgLayer, mono *image.NRGBA
	if ic.Foreground != "" {
		if fgLayer, err = loadIcon(ic.Foreground); err != nil {
			return err
		}
		if !hasTransparency(fgLayer) {
			fmt.Fprintf(os.Stderr, "asuka: icon foreground %s has no transparency: the launcher shows it as a square, its background unseen\n", ic.Foreground)
		}
	}
	if ic.Background != "" && !ic.backgroundColor() {
		if bgLayer, err = loadIcon(ic.Background); err != nil {
			return err
		}
	}
	if ic.Monochrome != "" {
		if mono, err = loadIcon(ic.Monochrome); err != nil {
			return err
		}
		if !hasTransparency(mono) {
			fmt.Fprintf(os.Stderr, "asuka: icon monochrome %s has no transparency: a themed icon is its alpha, so it shows as a square\n", ic.Monochrome)
		}
	}
	for _, d := range androidDensities {
		dir := filepath.Join(res, "mipmap-"+d.dir)
		if err := writePNG(filepath.Join(dir, name+".png"), resize(icon, int(48*d.scale))); err != nil {
			return err
		}
		// 108dp, the layer; the mask shows its middle 72dp at least.
		canvas := int(108 * d.scale)
		var fg *image.NRGBA
		if fgLayer != nil {
			fg = resize(fgLayer, canvas)
		} else {
			// The icon in the middle 72dp.
			fg = newNRGBA(canvas)
			inner := resize(icon, int(72*d.scale))
			off := (canvas - inner.Bounds().Dx()) / 2
			for y := 0; y < inner.Bounds().Dy(); y++ {
				for x := 0; x < inner.Bounds().Dx(); x++ {
					fg.SetNRGBA(x+off, y+off, inner.NRGBAAt(x, y))
				}
			}
		}
		if err := writePNG(filepath.Join(dir, name+"_foreground.png"), fg); err != nil {
			return err
		}
		if bgLayer != nil {
			if err := writePNG(filepath.Join(dir, name+"_background.png"), resize(bgLayer, canvas)); err != nil {
				return err
			}
		}
		if mono != nil {
			if err := writePNG(filepath.Join(dir, name+"_monochrome.png"), resize(mono, canvas)); err != nil {
				return err
			}
		}
	}
	background := "@mipmap/" + name + "_background"
	if bgLayer == nil {
		bg := icon.NRGBAAt(0, 0)
		if bg.A < 255 {
			bg = color.NRGBA{255, 255, 255, 255}
		}
		if ic.backgroundColor() {
			bg.R, bg.G, bg.B = rgb(ic.Background)
		}
		values := filepath.Join(res, "values")
		os.MkdirAll(values, 0o755)
		bgName := "asuka_icon_background"
		if name != "ic_launcher" {
			bgName = "asuka_" + name + "_background"
		}
		if err := os.WriteFile(filepath.Join(values, "asuka_"+name+".xml"), []byte(fmt.Sprintf(`<resources>
    <color name="%s">#%02X%02X%02X</color>
</resources>
`, bgName, bg.R, bg.G, bg.B)), 0o644); err != nil {
			return err
		}
		background = "@color/" + bgName
	}
	monochrome := ""
	if mono != nil {
		monochrome = fmt.Sprintf("    <monochrome android:drawable=\"@mipmap/%s_monochrome\" />\n", name)
	}
	anydpi := filepath.Join(res, "mipmap-anydpi-v26")
	os.MkdirAll(anydpi, 0o755)
	return os.WriteFile(filepath.Join(anydpi, name+".xml"), []byte(fmt.Sprintf(`<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="%s" />
    <foreground android:drawable="@mipmap/%s_foreground" />
%s</adaptive-icon>
`, background, name, monochrome)), 0o644)
}
