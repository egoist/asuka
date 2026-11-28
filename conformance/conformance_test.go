package conformance

import (
	"image"
	"image/color"
	"math"
	"os"
	"strings"
	"testing"

	"github.com/egoist/asuka/lang/compile"
)

// canvas is a synthetic capture: page p of the sheet at (ox, oy) pixels,
// k pixels per point, painted by f from each pixel's center in the
// sheet's points, its swatches their colors in sRGB.
func canvas(p, w, h int, ox, oy, k float64, f func(x, y float64) color.RGBA) *image.RGBA {
	return canvasIn(p, w, h, ox, oy, k, f, func(c RGB) color.RGBA { return color.RGBA{uint8(c.R), uint8(c.G), uint8(c.B), 255} })
}

// canvasIn is canvas with its swatches as space shows the cases' colors.
func canvasIn(p, w, h int, ox, oy, k float64, f func(x, y float64) color.RGBA, space func(RGB) color.RGBA) *image.RGBA {
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	sw, sh := SheetSize(p)
	for py := range h {
		for px := range w {
			x, y := (float64(px)+0.5-ox)/k, (float64(py)+0.5-oy)/k
			c := color.RGBA{40, 40, 40, 255} // the screen around the sheet
			if x >= 0 && y >= 0 && x < float64(sw) && y < float64(sh) {
				c = color.RGBA{255, 255, 255, 255}
				swatch, mark := -1, false
				for i := range swatches {
					if sx := float64(swatchX(i)); y < marker && x >= sx && x < sx+marker {
						swatch = i
					}
				}
				for j := range p + 1 {
					if mx := float64(pageMarkX(j)); y < marker && x >= mx && x < mx+marker {
						mark = true
					}
				}
				if (x < marker && y < marker) || (x >= float64(sw)-marker && y >= float64(sh)-marker) {
					c = color.RGBA{255, 0, 255, 255}
				} else if mark {
					c = color.RGBA{0, 0, 0, 255}
				} else if swatch >= 0 {
					c = space(swatches[swatch].rgb)
				} else if f != nil {
					c = f(x, y)
				}
			}
			img.SetRGBA(px, py, c)
		}
	}
	return img
}

func TestLocate(t *testing.T) {
	for p := range Pages() {
		for _, k := range []float64{1, 1.5, 2, 2.625, 3} {
			ox, oy := 17.0, 61.0
			sw, sh := SheetSize(p)
			img := canvas(p, int(ox+float64(sw)*k)+20, int(oy+float64(sh)*k)+30, ox, oy, k, nil)
			s, err := locate(img)
			if err != nil {
				t.Fatalf("page %d at %g×: %v", p+1, k, err)
			}
			if s.page != p || s.ox != ox || s.oy != oy || math.Abs(s.sx-k) > 0.01 || math.Abs(s.sy-k) > 0.01 {
				t.Errorf("page %d at %g×: found page %d at (%g,%g), %g×%g", p+1, k, s.page+1, s.ox, s.oy, s.sx, s.sy)
			}
		}
	}
	// A page's marks with another page's height: not the sheet.
	if Pages() > 1 {
		sw, sh := SheetSize(0)
		img := canvas(0, int(sw)+10, int(sh)+10, 3, 5, 1, nil)
		for y := range 8 {
			for x := range 8 {
				img.SetRGBA(3+int(pageMarkX(1))+x, 5+y, color.RGBA{0, 0, 0, 255})
			}
		}
		if s, err := locate(img); err == nil {
			t.Errorf("found page %d in page 1's sheet with page 2's marks", s.page+1)
		}
	}
	if _, err := locate(image.NewRGBA(image.Rect(0, 0, 50, 50))); err == nil {
		t.Error("found a sheet in an empty capture")
	}
}

// probeOn is a probe on case 0's cell of a synthetic capture at k×,
// painted by f in the cell's points.
func probeOn(t *testing.T, k float64, f func(x, y float64) color.RGBA) *Probe {
	t.Helper()
	sw, sh := SheetSize(0)
	img := canvas(0, int(float64(sw)*k)+10, int(float64(sh)*k)+10, 3, 5, k, func(x, y float64) color.RGBA {
		return f(x-marker, y-marker)
	})
	s, err := locate(img)
	if err != nil {
		t.Fatal(err)
	}
	return &Probe{s: s, x: marker, y: marker}
}

func TestEdge(t *testing.T) {
	for _, k := range []float64{1, 1.5, 2, 3} {
		// A red box from 20.25 to 80, its edge pixels mixed by coverage.
		p := probeOn(t, k, func(x, y float64) color.RGBA {
			px := 1 / k
			cov := math.Max(0, math.Min(1, (x+px/2-20.25)/px)) * math.Max(0, math.Min(1, (80-(x-px/2))/px))
			g := uint8(math.Round(255 * (1 - cov)))
			return color.RGBA{255, g, g, 255}
		})
		p.Edge("left", 0, 50, 50, 50, white, red, 20.25)
		p.Edge("right", 99, 50, 50, 50, white, red, 80)
		if len(p.errors) > 0 {
			t.Errorf("at %g×: %v", k, p.errors)
		}
		p.errors = nil
		p.Edge("left", 0, 50, 50, 50, white, red, 22)
		if len(p.errors) != 1 {
			t.Errorf("at %g×: an edge 1.75 points off passed", k)
		}
	}
}

// A capture in another color space than sRGB (the Mac's window server
// blends in Display P3's): a mix is measured between the colors as its
// swatches show them, so half way there is 0.5, not what sRGB's red
// would make it.
func TestMixInTheCapturesSpace(t *testing.T) {
	p3Red := color.RGBA{234, 51, 35, 255}  // sRGB's red in Display P3
	half := color.RGBA{245, 153, 145, 255} // half of it over white, blended there
	sw, sh := SheetSize(0)
	k := 2.0
	img := canvasIn(0, int(float64(sw)*k)+10, int(float64(sh)*k)+10, 3, 5, k, func(x, y float64) color.RGBA {
		if y < marker {
			return color.RGBA{255, 255, 255, 255} // the margin
		}
		return half
	}, func(c RGB) color.RGBA {
		if c == red {
			return p3Red
		}
		return color.RGBA{uint8(c.R), uint8(c.G), uint8(c.B), 255}
	})
	s, err := locate(img)
	if err != nil {
		t.Fatal(err)
	}
	p := &Probe{s: s, x: marker, y: marker}
	if got := p.Mix("half", 50, 50, white, red, 0.5, 0.02); len(p.errors) > 0 {
		t.Errorf("%.3f: %v", got, p.errors)
	}
	if got := p.At(50, 50).Mix(white, red); math.Abs(got-0.5) < 0.03 {
		t.Errorf("against sRGB's red the mix is %.3f: the test shows nothing", got)
	}
}

func TestBlur(t *testing.T) {
	for _, k := range []float64{1, 2, 3} {
		for _, sigma := range []float64{2, 4} {
			// A black shadow's edge at 60, blurred by a Gaussian.
			p := probeOn(t, k, func(x, y float64) color.RGBA {
				d := 0.5 * math.Erfc((x-60)/(sigma*math.Sqrt2))
				g := uint8(math.Round(255 * (1 - d)))
				return color.RGBA{g, g, g, 255}
			})
			e, s, plateau, ok := p.Blur(30, 50, 99, 50)
			if !ok || math.Abs(e-60) > 0.3 || math.Abs(s-sigma) > 0.25 || plateau < 0.99 {
				t.Errorf("at %g×, σ %g: edge %.2f, σ %.2f, plateau %.2f, ok %v", k, sigma, e, s, plateau, ok)
			}
		}
	}
}

func TestCasesFit(t *testing.T) {
	seen := map[string]bool{}
	for _, c := range Cases {
		if seen[c.Name] {
			t.Errorf("two cases named %q", c.Name)
		}
		seen[c.Name] = true
	}
	// Every case is on a page, once.
	n := 0
	for p := range Pages() {
		if len(pageCases(p)) > PerPage {
			t.Errorf("page %d has %d cases", p+1, len(pageCases(p)))
		}
		n += len(pageCases(p))
		// Each page fits a phone's screen in portrait, and the macOS
		// host's default window.
		if w, h := SheetSize(p); w > 360 || h > 580 {
			t.Errorf("page %d of the sheet is %gx%g points", p+1, w, h)
		}
		// Its marks are in the top margin, left of the top right corner.
		if x := pageMarkX(p + 1); x+marker > 2*marker+Columns*Cell-marker {
			t.Errorf("page %d's marks leave the top margin", p+1)
		}
	}
	if n != len(Cases) {
		t.Errorf("the pages show %d of the %d cases", n, len(Cases))
	}
}

// Measure reads which page a capture shows and runs that page's cases,
// each named, on it.
func TestMeasurePages(t *testing.T) {
	for p := range Pages() {
		sw, sh := SheetSize(p)
		img := canvas(p, int(sw)*2+10, int(sh)*2+10, 3, 5, 2, nil)
		page, results, err := Measure(img, "")
		if err != nil {
			t.Fatalf("page %d: %v", p+1, err)
		}
		if page != p || len(results) != len(pageCases(p)) {
			t.Fatalf("page %d: measured page %d, %d results", p+1, page+1, len(results))
		}
		for i, r := range results {
			if r.Case != pageCases(p)[i].Name {
				t.Errorf("page %d: result %d is %q, want %q", p+1, i, r.Case, pageCases(p)[i].Name)
			}
		}
	}
}

// The app's source is what App says (go generate ./conformance), and it
// compiles.
func TestApp(t *testing.T) {
	b, err := os.ReadFile("app/app.asuka")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasSuffix(string(b), App()) {
		t.Fatal("conformance/app/app.asuka is stale: go generate ./conformance")
	}
	res := compile.Files(map[string][]byte{"app.asuka": b}, nil)
	if err := res.Err(); err != nil {
		t.Fatalf("the sheet's app doesn't compile:\n%v", err)
	}
}
