#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include "scenepal.h"

/* True when black is 0, the five scene colours are distinct and above every pack index, and all fit one byte. */
static bool layout_ok(int bg, const int scene[5])
{
    if (bg != 0) {
        return false;
    }
    for (int i = 0; i < 5; i++) {
        if (scene[i] <= AP_MAX_COLORS || scene[i] > 255) {
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (scene[i] == scene[j]) {
                return false;
            }
        }
    }
    return true;
}

int main(void)
{
    const int scene[5] = {PAL_MASCOT, PAL_FILL, PAL_INK, PAL_ERROR, PAL_OFFLINE_FILL};
    assert(layout_ok(PAL_BG, scene));
    assert(PAL_PACK_FIRST == 1 && PAL_MASCOT == 251 && PAL_OFFLINE_FILL == 255);
    /* Red proofs: the old numbering puts the scene colours inside the pack range; a repeated name collides. */
    const int old[5] = {1, 2, 3, 4, 5}, twice[5] = {251, 252, 253, 253, 255};
    assert(!layout_ok(0, old));
    assert(!layout_ok(0, twice));
    puts("scenepal_test ok");
    return 0;
}
