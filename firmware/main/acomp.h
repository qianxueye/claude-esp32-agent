#pragma once
#include <stdint.h>
#include "rotblit.h"

/* Composes the rotated sticker art under a flush chunk: a chunk pixel that is 0 and inside the art takes pal[index]. Pure C, no static state. */

typedef struct {
    const uint8_t *buf;  /* decoded crop, w x h one-byte palette indices, index 0 transparent */
    int w, h, stride;    /* stride in bytes between crop rows, at least w */
    int scale;           /* 1 or 2 scene pixels per index */
    int ox, oy;          /* scene position of the crop's top-left pixel, not clipped */
    int tenths;          /* rotation in tenths of a degree, as rotblit */
    int scene, out;      /* scene square the art is placed in, and the output square */
} acomp_art_t;

/* Fills spans[a->out] with the output pixels the art covers, clamped to the output; all empty for a crop that is empty or has another scale. */
void acomp_spans(const acomp_art_t *a, xspan_t *spans);
/* Writes pal[index] into the zero pixels of dst inside spans; dst[0] is output pixel (x0, y0), x1 and y1 exclusive, so any chunk split gives the whole frame. */
void acomp_rows(const acomp_art_t *a, const xspan_t *spans, const uint16_t *pal, uint16_t *dst, int stride_px, int x0, int y0,
                int x1, int y1);
/* Per-row union of the previous and the new spans, n rows; an empty new set gives the previous one. out may alias either input. */
void acomp_dirty(const xspan_t *prev, const xspan_t *next, xspan_t *out, int n);
/* Strips of band_rows over the non-empty rows, each with the x-extent of its spans; stores at most max, returns the number that exist. */
int acomp_bands(const xspan_t *spans, int n, int band_rows, irect_t *out_rects, int max);
