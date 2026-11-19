#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"
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

#define ACOMP_ROWS SCENE_OUT_PX /* output rows the spans cover */
#define ACOMP_MAX_BANDS ((LCD_SIZE + DIRTY_BAND_ROWS - 1) / DIRTY_BAND_ROWS) /* derived: strips of DIRTY_BAND_ROWS over the whole screen */
_Static_assert(ACOMP_MAX_BANDS * DIRTY_BAND_ROWS >= LCD_SIZE && ACOMP_ROWS == LCD_SIZE, "the strip array holds every strip of a full-height union");

/* The art layer's state between ticks; art holds the spans the flush composes, the rest is each tick's output. */
typedef struct {
    xspan_t art[ACOMP_ROWS];   /* spans of the art now shown; empty when none */
    xspan_t dirty[ACOMP_ROWS]; /* union of the previous and the new art spans */
    xspan_t all[ACOMP_ROWS];   /* scratch: per-row union of dirty and the scene's spans */
    irect_t bands[ACOMP_MAX_BANDS];
    int n_bands;               /* strips of the union that exist; the caller invalidates the whole screen above ACOMP_MAX_BANDS */
} acomp_state_t;

/*
 * One tick: sets st->art to the spans of art when active (and art->out is ACOMP_ROWS), else empty, then the dirty union with
 * the previous spans, and the strips over that union and the scene's spans (NULL for none). Returns the strip count.
 */
int acomp_step(acomp_state_t *st, bool active, const acomp_art_t *art, const xspan_t *scene);
/* Chunk rows y0..y1 that hold span rows, as [*ya, *yb), and the split row between them for two cores; false when none. */
bool acomp_split(const xspan_t *spans, int y0, int y1, int *ya, int *mid, int *yb);
