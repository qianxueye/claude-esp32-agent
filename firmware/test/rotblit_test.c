#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rotblit.h"

#define S 480
#define OUT 466

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

static uint8_t g_src[S * S];
static uint16_t g_pal[256];
static uint16_t g_dst[OUT * OUT];
static xspan_t g_draw[OUT], g_clear[OUT];

static void fixture(void)
{
    uint32_t r = 12345u;
    for (int i = 0; i < S * S; i++) {
        r = r * 1664525u + 1013904223u;
        g_src[i] = (uint8_t)(1 + ((r >> 24) % 12));
    }
    for (int i = 0; i < 256; i++) {
        r = r * 1664525u + 1013904223u;
        g_pal[i] = (uint16_t)(r >> 16);
    }
    g_pal[0] = 0x1111;
}

typedef struct {
    int tenths;
    uint32_t spans, pixels;
} pin_t;

/* Pinned from rotblit.c as it stands; any change to a covered function body fails here. */
static const pin_t PINS[] = {
    {0, 0x4553142Du, 0xBFA2AAEFu},    {450, 0x947E97B8u, 0x18833890u},  {900, 0x609FD81Du, 0xF93B25F7u},
    {1234, 0xE09060DBu, 0xFC2D8F84u}, {1800, 0x0D952DEDu, 0xBE8A9C67u}, {2700, 0xEA03DF9Du, 0xB6663387u},
    {3599, 0x5707DC1Du, 0xCE86B296u},
};

int main(void)
{
    fixture();
    const irect_t inner = {40, 60, 440, 420}, outer = {20, 30, 470, 450};
    const int n = (int)(sizeof PINS / sizeof PINS[0]);
    for (int i = 0; i < n; i++) {
        const int t = PINS[i].tenths;
        rotblit_spans(&inner, S, OUT, t, g_draw);
        rotblit_spans(&outer, S, OUT, t, g_clear);
        uint32_t hs = fnv(2166136261u, g_draw, sizeof g_draw);
        hs = fnv(hs, g_clear, sizeof g_clear);
        memset(g_dst, 0, sizeof g_dst);
        rotblit(g_src, S, g_pal, g_dst, OUT, OUT, t, g_draw, g_clear, 0, OUT);
        const uint32_t hp = fnv(2166136261u, g_dst, sizeof g_dst);
        assert(hs == PINS[i].spans && hp == PINS[i].pixels);
    }
    printf("R1: %d angles pinned\n", n);
    return 0;
}
