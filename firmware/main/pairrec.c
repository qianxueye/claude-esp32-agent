#include "pairrec.h"
#include <string.h>

size_t pr_encode(uint32_t id, const void *data, size_t len, size_t max, uint8_t *out, size_t cap)
{
    if (id == 0 || len > max || len > 0xFFFF || cap < PR_HEADER_BYTES + len) {
        return 0;
    }
    out[0] = PR_VERSION;
    for (int i = 0; i < 4; i++) {
        out[1 + i] = (uint8_t)(id >> (8 * i));
    }
    out[5] = (uint8_t)len;
    out[6] = (uint8_t)(len >> 8);
    memcpy(out + PR_HEADER_BYTES, data, len);
    return PR_HEADER_BYTES + len;
}

bool pr_decode(const uint8_t *blob, size_t n, size_t max, uint32_t *id, const uint8_t **data, size_t *len)
{
    if (n < PR_HEADER_BYTES || blob[0] != PR_VERSION) {
        return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v |= (uint32_t)blob[1 + i] << (8 * i);
    }
    size_t l = (size_t)blob[5] | (size_t)blob[6] << 8;
    if (v == 0 || l > max || n != PR_HEADER_BYTES + l) {
        return false;
    }
    *id = v;
    *data = blob + PR_HEADER_BYTES;
    *len = l;
    return true;
}

uint32_t pr_next_id(uint32_t pair_id, uint32_t remote_id)
{
    uint32_t next = (pair_id > remote_id ? pair_id : remote_id) + 1;
    return next ? next : 1;
}

const char *pr_effective_remote(uint32_t pair_id, const netcfg_t *cfg, uint32_t remote_id, const char *remote_url)
{
    if (pair_id != 0 && remote_url && remote_id == pair_id) {
        return remote_url;
    }
    return cfg->has_remote ? cfg->remote_url : NULL;
}

pr_remote_plan_t pr_remote_plan(const char *stored, const char *in_use, const char *url)
{
    pr_remote_plan_t p;
    p.save = !stored || strcmp(stored, url) != 0;
    p.apply = !in_use || strcmp(in_use, url) != 0;
    return p;
}

bool pr_save_current(uint32_t stored_id, uint32_t want_id, uint32_t gen, uint32_t want_gen)
{
    return want_id != 0 && stored_id == want_id && gen == want_gen;
}
