package main

import (
	"bytes"
	"fmt"
	"image"
	"image/color"
	"image/draw"
	"image/png"
	"math"
	"os"
)

// A flow's screenshots are compared with their goldens as people see
// them, as pixelmatch (Mapbox's) does: two pixels differ when their
// colors are further apart than threshold in YIQ, the space that
// weighs brightness over hue as the eye does; a differing pixel on an
// edge drawn smoother in one image than the other (anti-aliasing: a
// font's rasterizer, a GPU's) doesn't count; and the images match when
// at most maxRatio of their pixels differ.

// imageCompare says how close two screenshots must be.
type imageCompare struct {
	threshold float64 // a pixel's difference, 0 to 1 (pixelmatch's 0.1)
	maxRatio  float64 // the share of pixels that may differ (0.001: 0.1%)
}

var defaultImageCompare = imageCompare{threshold: 0.1, maxRatio: 0.001}

// imageDiff is what a comparison found: the pixels that differ, those
// that differ only by anti-aliasing, and a picture of them (the golden
// faded, what differs red, anti-aliasing yellow).
type imageDiff struct {
	differ, aliased, total int
	diff                   *image.NRGBA
}

func (d imageDiff) ratio() float64 {
	if d.total == 0 {
		return 0
	}
	return float64(d.differ) / float64(d.total)
}

// readPNG reads a PNG as NRGBA.
func readPNG(path string) (*image.NRGBA, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return decodeNRGBA(b)
}

func decodeNRGBA(b []byte) (*image.NRGBA, error) {
	img, err := png.Decode(bytes.NewReader(b))
	if err != nil {
		return nil, err
	}
	return toNRGBA(img), nil
}

func toNRGBA(img image.Image) *image.NRGBA {
	if n, ok := img.(*image.NRGBA); ok && n.Rect.Min == (image.Point{}) {
		return n
	}
	r := img.Bounds()
	n := image.NewNRGBA(image.Rect(0, 0, r.Dx(), r.Dy()))
	draw.Draw(n, n.Rect, img, r.Min, draw.Src)
	return n
}

// samePixels reports whether two images are the same, pixel for pixel.
func samePixels(a, b *image.NRGBA) bool {
	return a.Rect == b.Rect && bytes.Equal(a.Pix, b.Pix)
}

// compareImages compares got with want; they must be the same size.
func compareImages(want, got *image.NRGBA, o imageCompare) (imageDiff, error) {
	if want.Rect.Size() != got.Rect.Size() {
		return imageDiff{}, fmt.Errorf("the golden is %dx%d, the screen %dx%d", want.Rect.Dx(), want.Rect.Dy(), got.Rect.Dx(), got.Rect.Dy())
	}
	w, h := want.Rect.Dx(), want.Rect.Dy()
	d := imageDiff{total: w * h, diff: image.NewNRGBA(want.Rect)}
	// 35215 is the YIQ distance from black to white.
	max := 35215 * o.threshold * o.threshold
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			i := want.PixOffset(x, y)
			delta := colorDelta(want.Pix[i:i+4], got.Pix[i:i+4], false)
			switch {
			case math.Abs(delta) > max:
				if antialiased(want, x, y, got) || antialiased(got, x, y, want) {
					d.aliased++
					d.diff.SetNRGBA(x, y, color.NRGBA{255, 255, 0, 255})
				} else {
					d.differ++
					d.diff.SetNRGBA(x, y, color.NRGBA{255, 0, 0, 255})
				}
			default:
				// The golden, faded to gray.
				l := blend(grayOf(want.Pix[i:i+4]), 0.1*float64(want.Pix[i+3])/255)
				c := uint8(l)
				d.diff.SetNRGBA(x, y, color.NRGBA{c, c, c, 255})
			}
		}
	}
	return d, nil
}

// blend mixes c with white by a (a pixel's alpha over a white page).
func blend(c, a float64) float64 { return 255 + (c-255)*a }

func grayOf(p []uint8) float64 {
	return rgbToY(float64(p[0]), float64(p[1]), float64(p[2]))
}

func rgbToY(r, g, b float64) float64 { return r*0.29889531 + g*0.58662247 + b*0.11448223 }
func rgbToI(r, g, b float64) float64 { return r*0.59597799 - g*0.27417610 - b*0.32180189 }
func rgbToQ(r, g, b float64) float64 { return r*0.21147017 - g*0.52261711 + b*0.31114694 }

// colorDelta is two pixels' squared YIQ distance (pixelmatch's), signed
// by which is brighter; yOnly: their brightness alone.
func colorDelta(p, q []uint8, yOnly bool) float64 {
	r1, g1, b1, a1 := float64(p[0]), float64(p[1]), float64(p[2]), float64(p[3])
	r2, g2, b2, a2 := float64(q[0]), float64(q[1]), float64(q[2]), float64(q[3])
	if a1 == a2 && r1 == r2 && g1 == g2 && b1 == b2 {
		return 0
	}
	if a1 < 255 {
		a := a1 / 255
		r1, g1, b1 = blend(r1, a), blend(g1, a), blend(b1, a)
	}
	if a2 < 255 {
		a := a2 / 255
		r2, g2, b2 = blend(r2, a), blend(g2, a), blend(b2, a)
	}
	y1, y2 := rgbToY(r1, g1, b1), rgbToY(r2, g2, b2)
	y := y1 - y2
	if yOnly {
		return y
	}
	i := rgbToI(r1, g1, b1) - rgbToI(r2, g2, b2)
	q2 := rgbToQ(r1, g1, b1) - rgbToQ(r2, g2, b2)
	delta := 0.5053*y*y + 0.299*i*i + 0.1957*q2*q2
	if y1 > y2 {
		return -delta
	}
	return delta
}

// antialiased reports whether img's pixel at x, y is likely an
// anti-aliased edge (pixelmatch's, after Vysniauskas's "Anti-aliased
// Pixel and Intensity Slope Detector"): among its 8 neighbors it has
// at most two of its own brightness, and of the brightest and darkest
// of them, one lies in a flat area of both images.
func antialiased(img *image.NRGBA, x1, y1 int, other *image.NRGBA) bool {
	w, h := img.Rect.Dx(), img.Rect.Dy()
	x0, y0 := max(x1-1, 0), max(y1-1, 0)
	x2, y2 := min(x1+1, w-1), min(y1+1, h-1)
	zeroes := 0
	if x1 == x0 || x1 == x2 || y1 == y0 || y1 == y2 {
		zeroes = 1 // on the image's edge
	}
	var minD, maxD float64
	var minX, minY, maxX, maxY int
	c := img.PixOffset(x1, y1)
	for x := x0; x <= x2; x++ {
		for y := y0; y <= y2; y++ {
			if x == x1 && y == y1 {
				continue
			}
			n := img.PixOffset(x, y)
			delta := colorDelta(img.Pix[c:c+4], img.Pix[n:n+4], true)
			switch {
			case delta == 0:
				zeroes++
				if zeroes > 2 {
					return false
				}
			case delta < minD:
				minD, minX, minY = delta, x, y
			case delta > maxD:
				maxD, maxX, maxY = delta, x, y
			}
		}
	}
	if minD == 0 || maxD == 0 {
		return false
	}
	return (hasManySiblings(img, minX, minY) && hasManySiblings(other, minX, minY)) ||
		(hasManySiblings(img, maxX, maxY) && hasManySiblings(other, maxX, maxY))
}

// hasManySiblings reports whether at least three of a pixel's neighbors
// are the same color as it: a flat area.
func hasManySiblings(img *image.NRGBA, x1, y1 int) bool {
	w, h := img.Rect.Dx(), img.Rect.Dy()
	x0, y0 := max(x1-1, 0), max(y1-1, 0)
	x2, y2 := min(x1+1, w-1), min(y1+1, h-1)
	zeroes := 0
	if x1 == x0 || x1 == x2 || y1 == y0 || y1 == y2 {
		zeroes = 1
	}
	c := img.PixOffset(x1, y1)
	for x := x0; x <= x2; x++ {
		for y := y0; y <= y2; y++ {
			if x == x1 && y == y1 {
				continue
			}
			n := img.PixOffset(x, y)
			if bytes.Equal(img.Pix[c:c+4], img.Pix[n:n+4]) {
				zeroes++
			}
			if zeroes > 2 {
				return true
			}
		}
	}
	return false
}
