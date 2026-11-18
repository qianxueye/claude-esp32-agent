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
