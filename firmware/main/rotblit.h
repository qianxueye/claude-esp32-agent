#pragma once
#include <stdint.h>

typedef struct {
    int x0, y0, x1, y1; /* x1, y1 exclusive */
} irect_t;

typedef struct {
    int16_t x0, x1; /* x1 exclusive; empty when x0 >= x1 */
} xspan_t;

/* Fills spans[dst_size] with the destination pixels covered by src_rect rotated clockwise by tenths of a degree. */
void rotblit_spans(const irect_t *src_rect, int src_size, int dst_size, int tenths, xspan_t *spans);
/*
 * Resamples a palette-indexed square source into the RGB565 destination rows [y0, y1): pixels inside draw[y] come from the
 * source, the rest of clear[y] is set to palette entry 0.
 */
void rotblit(const uint8_t *src, int src_size, const uint16_t *palette, uint16_t *dst, int dst_stride_px, int dst_size,
             int tenths, const xspan_t *draw, const xspan_t *clear, int y0, int y1);

#define ROTBLIT_NEAREST_INSET 0.015625f /* mathematical model: 1/64 px, above the worst 16.16 stepping and float start error over one row, tested by rotblit_test R2 */

/*
 * Spans of the output pixels whose centres map, nearest-sampled with no half-pixel offset, inside src_rect shrunk by
 * ROTBLIT_NEAREST_INSET; the rectangle may lie partly outside the source square, which only fixes the rotation centre.
 */
void rotblit_spans_nearest(const irect_t *src_rect, int src_size, int dst_size, int tenths, xspan_t *spans);
