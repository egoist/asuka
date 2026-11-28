package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"path/filepath"
)

// iconConfig is asuka.toml's icon: a square PNG's path,
//
//	icon = "icon.png"
//
// or a table of that image and the layers and appearances the platforms
// draw the icon from:
//
//	[icon]
//	image = "icon.png"
//	foreground = "icon-foreground.png"
//	background = "#1b2440"
//	monochrome = "icon-monochrome.png"
//	dark = "icon-dark.png"
//	tinted = "icon-tinted.png"
type iconConfig struct {
	// Image is the icon: a square PNG, 1024×1024, opaque for iOS. Every
	// platform's sizes are made from it, and Android's adaptive icon too
	// when there's no Foreground.
	Image string `json:"image"`
	// Foreground and Background are Android's adaptive icon's layers
	// (Android 8 and later): each a square PNG of the whole 108dp layer,
	// of which the launcher's mask shows the middle 72dp at least, the
	// middle 66dp always; Background may be a color, "#rrggbb", instead.
	// Default: Image in the middle of the layer, over its corner's color.
	Foreground string `json:"foreground"`
	Background string `json:"background"`
	// Monochrome is Android 13's themed icon's layer: a square PNG of the
	// 108dp layer whose alpha the launcher tints with the wallpaper's
	// colors (default: none, so a themed launcher shows the icon as is).
	Monochrome string `json:"monochrome"`
	// Dark and Tinted are iOS 18's appearances of the icon on the home
	// screen: a square PNG, 1024×1024, for dark mode (its background
	// transparent: iOS draws its own behind it), and a grayscale one iOS
	// tints (default: iOS makes them from Image).
	Dark   string `json:"dark"`
	Tinted string `json:"tinted"`
}

func (ic *iconConfig) UnmarshalJSON(b []byte) error {
	if b = bytes.TrimSpace(b); len(b) > 0 && b[0] == '"' {
		return json.Unmarshal(b, &ic.Image)
	}
	if len(b) == 0 || b[0] != '{' {
		return errors.New("icon: want a PNG's path, or a table of its image and layers")
	}
	type plain iconConfig
	var p plain
	if err := json.Unmarshal(b, &p); err != nil {
		return err
	}
	*ic = iconConfig(p)
	return nil
}

// set reports whether asuka.toml has an icon.
func (ic *iconConfig) set() bool { return *ic != iconConfig{} }

// resolve makes ic's paths the app's directory's, and checks it.
func (ic *iconConfig) resolve(dir string) error {
	if !ic.set() {
		return nil
	}
	if ic.Image == "" {
		return errors.New("asuka.toml: icon: the table needs its image, the icon every platform's sizes are made from")
	}
	for _, p := range []*string{&ic.Image, &ic.Foreground, &ic.Monochrome, &ic.Dark, &ic.Tinted} {
		if *p != "" && !filepath.IsAbs(*p) {
			*p = filepath.Join(dir, *p)
		}
	}
	if ic.Background != "" {
		if len(ic.Background) > 0 && ic.Background[0] == '#' {
			if !hexColor.MatchString(ic.Background) || len(ic.Background) != 7 {
				return fmt.Errorf("asuka.toml: icon background %q (want #rrggbb, or a PNG's path)", ic.Background)
			}
		} else if !filepath.IsAbs(ic.Background) {
			ic.Background = filepath.Join(dir, ic.Background)
		}
	}
	if ic.Background != "" && ic.Foreground == "" {
		return errors.New("asuka.toml: icon: background is the adaptive icon's back layer, which needs its foreground too")
	}
	return nil
}

// backgroundColor reports whether the adaptive icon's background is a
// color, not an image.
func (ic *iconConfig) backgroundColor() bool {
	return len(ic.Background) > 0 && ic.Background[0] == '#'
}
