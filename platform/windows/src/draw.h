// draw.h: what the drawing parts share among themselves (shape.c,
// canvas.c, image.c, shader.c, clip.c, picture.c, material.c).
#ifndef ASUKA_WIN_DRAW_H
#define ASUKA_WIN_DRAW_H

#include "host.h"

// shape.c
GpPath *wh_build_path(const uint8_t *b, size_t n, float k);
void wh_style_pen(GpPen *pen, int cap, int join, const float *dash, uint32_t ndash, float phase, float width);

// clip.c
double wh_clip_coverage(int x, int y, int w, int h, double rc);
// wh_clip_coverage4 is wh_clip_coverage's of corners of radii r (top
// left, top right, bottom right, bottom left).
double wh_clip_coverage4(int x, int y, int w, int h, const double r[4]);
void wh_restore_shape(node *n);

// picture.c: a DIB section of w by h pixels (BGRA, top-down) and its
// pixels; whether n's window is shaped to nothing under its picture.
HBITMAP wh_dib(int w, int h, uint8_t **px);
bool wh_picture_shaped(const node *n);

// image.c: RGBA, not premultiplied, as premultiplied BGRA (in place).
void wh_premultiply_rgba(uint8_t *px, size_t n);

#endif
