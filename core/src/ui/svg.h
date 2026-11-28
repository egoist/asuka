// svg.h: SVG documents read at run time (svg.c): std/svg's Document and
// Image's .svg sources, drawn as a Canvas's commands, the same on every
// platform.
#ifndef AK_SVG_H
#define AK_SVG_H

#include "ops/ops.h"

// ak_svg_parse reads SVG markup (UTF-8) into a document, written to out
// (emptied first): false, and err (NUL-terminated, at most 160 bytes)
// says why, if it isn't an SVG document it can read. Any thread: it keeps
// no state.
//
// A document is little-endian bytes: "ASVG", u8 version (1), f32 width,
// height (its size in points: the svg's width and height, else its
// viewBox's), f32 viewBox x, y, width, height, u8 its preserveAspectRatio's
// alignment (0 none, 1 to 9 xMinYMin, xMidYMin, xMaxYMin, xMinYMid, ...
// xMaxYMax), u8 1 if it slices (else it meets), u32 the number of colors
// that are currentColor and per one u32 its offset in the commands and f32
// its opacity, u32 the commands' length and the commands: a Canvas's
// (ops/schema.go's Canvas Commands), in the viewBox's units.
bool ak_svg_parse(const char *src, size_t n, ak_buf *out, char *err);

// ak_svg_size is a document's size in points (false: it isn't one).
bool ak_svg_size(const uint8_t *doc, size_t n, float *w, float *h);

// ak_svg_draw appends to out the commands that draw a document in a box
// of w by h points, fitted by mode (an ImageMode: its size, kept in
// proportion, fitted into the box or filling it, stretched to it, or as
// it is, centered), clipped to the box, its currentColor current, and
// every color the tint's (its own opacity times the tint's) unless tint
// is 0. False, out as it was, if doc isn't a document.
bool ak_svg_draw(const uint8_t *doc, size_t n, ak_buf *out, float w, float h, int mode, ak_color current, ak_color tint);

#endif
