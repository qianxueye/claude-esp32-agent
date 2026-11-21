#include "netcfg.h"
#include <stdio.h>
#include <string.h>

/* Strict JSON subset reader for the fixed PAIR and SET_REMOTE schemas; rules in docs/network.md "Configuration text". */

#define JSON_DEPTH_MAX 3
#define KEY_BUF 16
#define IPV4_TEXT_MAX 15
_Static_assert(JSON_DEPTH_MAX >= 3, "a networks entry sits at depth 3");

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    bool bad;
} jcur_t;

enum { JK_BAD, JK_STR, JK_ARR, JK_OBJ, JK_BOOL, JK_NUM, JK_NULL };

static void jws(jcur_t *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) {
        c->p++;
    }
}

static bool jeat(jcur_t *c, uint8_t ch)
{
    jws(c);
    if (c->p < c->end && *c->p == ch) {
        c->p++;
        return true;
    }
    return false;
}

static int jkind(jcur_t *c)
{
    jws(c);
    if (c->p >= c->end) {
        return JK_BAD;
    }
    uint8_t b = *c->p;
    if (b == '"') return JK_STR;
    if (b == '[') return JK_ARR;
    if (b == '{') return JK_OBJ;
    if (b == 't' || b == 'f') return JK_BOOL;
    if (b == 'n') return JK_NULL;
    if (b == '-' || (b >= '0' && b <= '9')) return JK_NUM;
    return JK_BAD;
}

static int hexval(uint8_t b)
{
    if (b >= '0' && b <= '9') return b - '0';
    if (b >= 'a' && b <= 'f') return b - 'a' + 10;
    if (b >= 'A' && b <= 'F') return b - 'A' + 10;
    return -1;
}

static void put_byte(char *dst, size_t cap, size_t *n, uint8_t b)
{
    if (dst && *n + 1 < cap) {
        dst[*n] = (char)b;
    }
    (*n)++;
}

static bool read_u4(jcur_t *c, uint32_t *cp)
{
    if (c->end - c->p < 4) {
        return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(c->p[i]);
        if (h < 0) {
            return false;
        }
        v = (v << 4) | (uint32_t)h;
    }
    c->p += 4;
    *cp = v;
    return true;
}

/* Length of a valid UTF-8 sequence starting at p, or 0. */
static size_t utf8_seq(const uint8_t *p, const uint8_t *end)
{
    uint8_t b = p[0];
    size_t n;
    uint8_t lo = 0x80, hi = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) {
        n = 2;
    } else if (b >= 0xE0 && b <= 0xEF) {
        n = 3;
        if (b == 0xE0) lo = 0xA0;
        if (b == 0xED) hi = 0x9F;
    } else if (b >= 0xF0 && b <= 0xF4) {
        n = 4;
        if (b == 0xF0) lo = 0x90;
        if (b == 0xF4) hi = 0x8F;
    } else {
        return 0;
    }
    if ((size_t)(end - p) < n || p[1] < lo || p[1] > hi) {
        return 0;
    }
    for (size_t i = 2; i < n; i++) {
        if (p[i] < 0x80 || p[i] > 0xBF) {
            return 0;
        }
    }
    return n;
}

/* Decodes the string at the cursor into dst (NULL to skip) and returns its decoded length.
 * dst keeps at most cap - 1 bytes and a terminator; a longer string is cut but still counted. */
static size_t jstring(jcur_t *c, char *dst, size_t cap)
{
    size_t n = 0;
    if (dst && cap) {
        dst[0] = 0;
    }
    if (jkind(c) != JK_STR) {
        c->bad = true;
        return 0;
    }
    c->p++;
    for (;;) {
        if (c->p >= c->end) {
            c->bad = true;
            return 0;
        }
        uint8_t b = *c->p;
        if (b == '"') {
            c->p++;
            break;
        }
        if (b < 0x20) {
            c->bad = true;
            return 0;
        }
        if (b >= 0x80) {
            size_t k = utf8_seq(c->p, c->end);
            if (!k) {
                c->bad = true;
                return 0;
            }
            for (size_t i = 0; i < k; i++) {
                put_byte(dst, cap, &n, c->p[i]);
            }
            c->p += k;
            continue;
        }
        c->p++;
        if (b != '\\') {
            put_byte(dst, cap, &n, b);
            continue;
        }
        if (c->p >= c->end) {
            c->bad = true;
            return 0;
        }
        uint8_t e = *c->p++;
        uint32_t cp;
        switch (e) {
        case '"': case '\\': case '/': cp = e; break;
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u':
            if (!read_u4(c, &cp)) {
                c->bad = true;
                return 0;
            }
            if (cp >= 0xDC00 && cp <= 0xDFFF) {
                c->bad = true;
                return 0;
            }
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                uint32_t lo;
                if (c->end - c->p < 2 || c->p[0] != '\\' || c->p[1] != 'u') {
                    c->bad = true;
                    return 0;
                }
                c->p += 2;
                if (!read_u4(c, &lo) || lo < 0xDC00 || lo > 0xDFFF) {
                    c->bad = true;
                    return 0;
                }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            break;
        default:
            c->bad = true;
            return 0;
        }
        if (cp == 0) {
            c->bad = true;
            return 0;
        }
        if (cp < 0x80) {
            put_byte(dst, cap, &n, (uint8_t)cp);
        } else if (cp < 0x800) {
            put_byte(dst, cap, &n, (uint8_t)(0xC0 | (cp >> 6)));
            put_byte(dst, cap, &n, (uint8_t)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            put_byte(dst, cap, &n, (uint8_t)(0xE0 | (cp >> 12)));
            put_byte(dst, cap, &n, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
            put_byte(dst, cap, &n, (uint8_t)(0x80 | (cp & 0x3F)));
        } else {
            put_byte(dst, cap, &n, (uint8_t)(0xF0 | (cp >> 18)));
            put_byte(dst, cap, &n, (uint8_t)(0x80 | ((cp >> 12) & 0x3F)));
            put_byte(dst, cap, &n, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
            put_byte(dst, cap, &n, (uint8_t)(0x80 | (cp & 0x3F)));
        }
    }
    if (dst && cap) {
        dst[n < cap ? n : cap - 1] = 0;
    }
    return n;
}

static bool is_digit(uint8_t b)
{
    return b >= '0' && b <= '9';
}

/* Lexes any JSON number; *uint_ok tells whether it is a non-negative integer, *v its value capped at UINT32_MAX. */
static void jnumber(jcur_t *c, uint32_t *v, bool *uint_ok)
{
    *v = 0;
    *uint_ok = true;
    if (c->p < c->end && *c->p == '-') {
        *uint_ok = false;
        c->p++;
    }
    if (c->p >= c->end || !is_digit(*c->p)) {
        c->bad = true;
        return;
    }
    if (*c->p == '0') {
        c->p++;
    } else {
        while (c->p < c->end && is_digit(*c->p)) {
            uint32_t d = (uint32_t)(*c->p++ - '0');
            *v = *v > (UINT32_MAX - d) / 10 ? UINT32_MAX : *v * 10 + d;
        }
    }
    if (c->p < c->end && *c->p == '.') {
        *uint_ok = false;
        c->p++;
        if (c->p >= c->end || !is_digit(*c->p)) {
            c->bad = true;
            return;
        }
        while (c->p < c->end && is_digit(*c->p)) c->p++;
    }
    if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
        *uint_ok = false;
        c->p++;
        if (c->p < c->end && (*c->p == '+' || *c->p == '-')) c->p++;
        if (c->p >= c->end || !is_digit(*c->p)) {
            c->bad = true;
            return;
        }
        while (c->p < c->end && is_digit(*c->p)) c->p++;
    }
}

static bool jliteral(jcur_t *c, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(c->end - c->p) < n || memcmp(c->p, lit, n) != 0) {
        c->bad = true;
        return false;
    }
    c->p += n;
    return true;
}

/* Reads true or false. */
static bool jbool(jcur_t *c)
{
    if (c->p < c->end && *c->p == 't') {
        return jliteral(c, "true");
    }
    jliteral(c, "false");
    return false;
}

static void jskip(jcur_t *c, int depth);

/* Walks an object or array whose container sits at depth; calls each member through skip. */
static void jskip_container(jcur_t *c, int depth, bool object)
{
    if (depth > JSON_DEPTH_MAX) {
        c->bad = true;
        return;
    }
    c->p++;
    if (jeat(c, object ? '}' : ']')) {
        return;
    }
    do {
        if (object) {
            jstring(c, NULL, 0);
            if (c->bad || !jeat(c, ':')) {
                c->bad = true;
                return;
            }
        }
        jskip(c, depth);
        if (c->bad) {
            return;
        }
    } while (jeat(c, ','));
    if (!jeat(c, object ? '}' : ']')) {
        c->bad = true;
    }
}

/* Skips one value inside a container at depth. */
static void jskip(jcur_t *c, int depth)
{
    uint32_t v;
    bool u;
    switch (jkind(c)) {
    case JK_STR: jstring(c, NULL, 0); break;
    case JK_ARR: jskip_container(c, depth + 1, false); break;
    case JK_OBJ: jskip_container(c, depth + 1, true); break;
    case JK_BOOL: jbool(c); break;
    case JK_NULL: jliteral(c, "null"); break;
    case JK_NUM: jnumber(c, &v, &u); break;
    default: c->bad = true; break;
    }
}

/* Reads a member key and its colon; returns false at a syntax error. */
static bool jkey(jcur_t *c, char key[KEY_BUF], size_t *klen)
{
    *klen = jstring(c, key, KEY_BUF);
    return !c->bad && jeat(c, ':');
}

static bool key_is(const char *key, size_t klen, const char *name)
{
    return klen < KEY_BUF && klen == strlen(name) && memcmp(key, name, klen) == 0;
}

/* Starts an array at the cursor; returns false when it is empty. Call jnext after each element. */
static bool jarr_begin(jcur_t *c, int depth)
{
    if (depth > JSON_DEPTH_MAX) {
        c->bad = true;
        return false;
    }
    c->p++;
    return !jeat(c, ']');
}

/* Moves past a separator; returns false at the closing bracket or on a syntax error. */
static bool jnext(jcur_t *c, uint8_t close)
{
    if (c->bad) {
        return false;
    }
    if (jeat(c, ',')) {
        return true;
    }
    if (!jeat(c, close)) {
        c->bad = true;
    }
    return false;
}

void netcfg_wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) {
        *v++ = 0;
    }
}

void netcfg_identity(const uint8_t mac[6], char out[NET_IDENTITY_LEN + 1])
{
    static const char hex[] = "0123456789ABCDEF";
    memcpy(out, "clawd-", 6);
    for (int i = 0; i < 6; i++) {
        out[6 + 2 * i] = hex[mac[i] >> 4];
        out[7 + 2 * i] = hex[mac[i] & 15];
    }
    out[NET_IDENTITY_LEN] = 0;
}

static bool host_char(uint8_t b)
{
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || is_digit(b) || b == '-' || b == '.';
}

bool netcfg_url_split(const char *url, size_t len, netcfg_url_t *parts)
{
    const uint8_t *p = (const uint8_t *)url, *end = p + len;
    if (len > NET_URL_MAX || len < 6 || memcmp(p, "wss://", 6) != 0) {
        return false;
    }
    p += 6;
    const uint8_t *host = p;
    while (p < end && host_char(*p)) p++;
    size_t hlen = (size_t)(p - host);
    if (!hlen) {
        return false;
    }
    uint32_t port = 443;
    if (p < end && *p == ':') {
        p++;
        const uint8_t *d = p;
        port = 0;
        while (p < end && is_digit(*p) && p - d < 5) port = port * 10 + (uint32_t)(*p++ - '0');
        if (p == d || port < 1 || port > 65535) {
            return false;
        }
    }
    if (p >= end || *p != '/') {
        return false;
    }
    for (const uint8_t *q = p; q < end; q++) {
        if (*q < 0x21 || *q > 0x7E) {
            return false;
        }
    }
    if (parts) {
        memcpy(parts->host, host, hlen);
        parts->host[hlen] = 0;
        parts->port = (uint16_t)port;
        parts->path = (const char *)p;
    }
    return true;
}

static bool ipv4_parse(const char *s, size_t n, uint8_t out[4])
{
    const char *p = s, *end = s + n;
    for (int i = 0; i < 4; i++) {
        if (i && (p >= end || *p++ != '.')) {
            return false;
        }
        const char *d = p;
        uint32_t v = 0;
        while (p < end && is_digit((uint8_t)*p) && p - d < 3) v = v * 10 + (uint32_t)(*p++ - '0');
        if (p == d || v > 255 || (p - d > 1 && *d == '0')) {
            return false;
        }
        out[i] = (uint8_t)v;
    }
    return p == end;
}

static bool password_ok(const char *s, size_t n)
{
    if (n == 0 || (n >= 8 && n <= 63)) {
        return true;
    }
    if (n != NET_PASS_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (hexval((uint8_t)s[i]) < 0) {
            return false;
        }
    }
    return true;
}

static bool ssid_ok(size_t n)
{
    return n >= 1 && n <= NET_SSID_MAX;
}

enum { PF_SSID, PF_PASSWORD, PF_IDENTITY, PF_PSK, PF_KEY_ID, PF_NAMES, PF_ADDRS, PF_PORT, PF_NETWORKS, PF_REMOTE, PF_COUNT };
enum { F_ABSENT, F_OK, F_BAD };

static const char *const PAIR_KEYS[PF_COUNT] = {
    "ssid", "password", "psk_identity", "psk", "key_id", "relay_names", "relay_addrs", "relay_port", "networks", "remote_url",
};
static const char *const PAIR_BAD_FIELD[PF_COUNT] = {
    "bad field ssid", "bad field password", "bad field psk_identity", "bad field psk", "bad field key_id", "bad field relay_names",
    "bad field relay_addrs", "bad field relay_port", "bad field networks", "bad field remote_url",
};

typedef struct {
    uint8_t f[PF_COUNT];
    netcfg_net_t top;
    netcfg_net_t spare;
    size_t ssid_len, pass_len, ident_len, psk_len, kid_len, url_len;
    char ident[NET_IDENTITY_LEN + 2];
    char kid[NET_KEY_ID_LEN + 2];
    char psk_hex[2 * NET_PSK_BYTES + 2];
    char addr_text[IPV4_TEXT_MAX + 2];
    char name_spare[NET_RELAY_NAME_MAX + 2];
    size_t n_nets, n_names, n_addrs;
    bool nets_ssid_bad, nets_pass_bad, relay_bad;
    uint32_t port;
} pair_scan_t;

static int pair_key(const char *key, size_t klen)
{
    for (int i = 0; i < PF_COUNT; i++) {
        if (key_is(key, klen, PAIR_KEYS[i])) {
            return i;
        }
    }
    return -1;
}

/* Reads one networks entry; returns false when its shape is wrong (any content fault is recorded in s). */
static bool pair_network(jcur_t *c, pair_scan_t *s, netcfg_net_t *net)
{
    if (jkind(c) != JK_OBJ) {
        jskip(c, 2);
        return false;
    }
    c->p++;
    uint8_t got_ssid = F_ABSENT, got_pass = F_ABSENT, got_lan = F_ABSENT;
    size_t ssid_len = 0, pass_len = 0;
    if (!jeat(c, '}')) {
        do {
            char key[KEY_BUF];
            size_t klen;
            if (!jkey(c, key, &klen)) {
                c->bad = true;
                return false;
            }
            int k = jkind(c);
            uint8_t *slot = key_is(key, klen, "ssid") ? &got_ssid
                : key_is(key, klen, "password") ? &got_pass
                : key_is(key, klen, "lan") ? &got_lan : NULL;
            if (!slot) {
                jskip(c, 3);
            } else if (*slot != F_ABSENT || k != (slot == &got_lan ? JK_BOOL : JK_STR)) {
                *slot = F_BAD;
                jskip(c, 3);
            } else if (slot == &got_ssid) {
                ssid_len = jstring(c, net->ssid, sizeof net->ssid);
                *slot = F_OK;
            } else if (slot == &got_pass) {
                pass_len = jstring(c, net->password, sizeof net->password);
                *slot = F_OK;
            } else {
                net->lan = jbool(c);
                *slot = F_OK;
            }
            if (c->bad) {
                return false;
            }
        } while (jeat(c, ','));
        if (!jeat(c, '}')) {
            c->bad = true;
            return false;
        }
    }
    if (got_ssid != F_OK || got_pass != F_OK || got_lan != F_OK) {
        return false;
    }
    if (!ssid_ok(ssid_len)) {
        s->nets_ssid_bad = true;
    }
    if (!password_ok(net->password, pass_len)) {
        s->nets_pass_bad = true;
    }
    return true;
}

static void pair_value(jcur_t *c, pair_scan_t *s, netcfg_t *out, int field)
{
    static const int kinds[PF_COUNT] = {JK_STR, JK_STR, JK_STR, JK_STR, JK_STR, JK_ARR, JK_ARR, JK_NUM, JK_ARR, JK_STR};
    int kind = jkind(c);
    if (s->f[field] != F_ABSENT || kind != kinds[field]) {
        s->f[field] = F_BAD;
        jskip(c, 1);
        return;
    }
    s->f[field] = F_OK;
    switch (field) {
    case PF_SSID: s->ssid_len = jstring(c, s->top.ssid, sizeof s->top.ssid); break;
    case PF_PASSWORD: s->pass_len = jstring(c, s->top.password, sizeof s->top.password); break;
    case PF_IDENTITY: s->ident_len = jstring(c, s->ident, sizeof s->ident); break;
    case PF_PSK: s->psk_len = jstring(c, s->psk_hex, sizeof s->psk_hex); break;
    case PF_KEY_ID: s->kid_len = jstring(c, s->kid, sizeof s->kid); break;
    case PF_REMOTE: s->url_len = jstring(c, out->remote_url, sizeof out->remote_url); break;
    case PF_PORT: {
        bool uint_ok;
        jnumber(c, &s->port, &uint_ok);
        if (!uint_ok) {
            s->f[field] = F_BAD;
        }
        break;
    }
    case PF_NAMES:
        if (jarr_begin(c, 2)) {
            do {
                if (jkind(c) != JK_STR) {
                    s->f[field] = F_BAD;
                    jskip(c, 2);
                    continue;
                }
                char *dst = s->n_names < NET_RELAY_NAMES_MAX ? out->names[s->n_names] : s->name_spare;
                size_t n = jstring(c, dst, s->n_names < NET_RELAY_NAMES_MAX ? NET_RELAY_NAME_MAX + 1 : sizeof s->name_spare);
                if (n == 0 || n > NET_RELAY_NAME_MAX) {
                    s->relay_bad = true;
                }
                s->n_names++;
            } while (jnext(c, ']'));
        }
        break;
    case PF_ADDRS:
        if (jarr_begin(c, 2)) {
            do {
                if (jkind(c) != JK_STR) {
                    s->f[field] = F_BAD;
                    jskip(c, 2);
                    continue;
                }
                size_t n = jstring(c, s->addr_text, sizeof s->addr_text);
                uint8_t ip[4];
                if (n > IPV4_TEXT_MAX || !ipv4_parse(s->addr_text, n, ip)) {
                    s->relay_bad = true;
                } else if (s->n_addrs < NET_RELAY_ADDRS_MAX) {
                    memcpy(out->addrs[s->n_addrs], ip, 4);
                }
                s->n_addrs++;
            } while (jnext(c, ']'));
        }
        break;
    case PF_NETWORKS:
        if (!jarr_begin(c, 2)) {
            if (!c->bad) {
                s->f[field] = F_BAD;
            }
            break;
        }
        do {
            netcfg_net_t *net = s->n_nets < WIFI_NETWORKS_MAX ? &out->nets[s->n_nets] : &s->spare;
            if (!pair_network(c, s, net)) {
                s->f[field] = F_BAD;
            }
            s->n_nets++;
        } while (jnext(c, ']'));
        break;
    default:
        break;
    }
}

static const char *pair_verdict(pair_scan_t *s, const char *identity, netcfg_t *out)
{
    for (int i = 0; i < PF_COUNT; i++) {
        bool optional = i == PF_NETWORKS || i == PF_REMOTE;
        if (s->f[i] == F_BAD || (s->f[i] == F_ABSENT && !optional)) {
            return PAIR_BAD_FIELD[i];
        }
    }
    if (s->psk_len != 2 * NET_PSK_BYTES) {
        return "bad psk";
    }
    for (size_t i = 0; i < NET_PSK_BYTES; i++) {
        int hi = hexval((uint8_t)s->psk_hex[2 * i]), lo = hexval((uint8_t)s->psk_hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return "bad psk";
        }
        out->psk[i] = (uint8_t)(hi << 4 | lo);
    }
    if (s->kid_len != NET_KEY_ID_LEN) {
        return "bad key_id";
    }
    for (size_t i = 0; i < NET_KEY_ID_LEN; i++) {
        uint8_t b = (uint8_t)s->kid[i];
        if (!((b >= '0' && b <= '9') || (b >= 'A' && b <= 'F'))) {
            return "bad key_id";
        }
    }
    if (s->ident_len != NET_IDENTITY_LEN || strlen(identity) != NET_IDENTITY_LEN
        || memcmp(s->ident, identity, NET_IDENTITY_LEN) != 0) {
        return "identity mismatch";
    }
    if (!ssid_ok(s->ssid_len)) {
        return "bad ssid";
    }
    if (!password_ok(s->top.password, s->pass_len)) {
        return "bad password";
    }
    if (s->f[PF_NETWORKS] == F_OK) {
        if (s->n_nets > WIFI_NETWORKS_MAX) {
            return "too many networks";
        }
        if (s->nets_ssid_bad) {
            return "bad ssid";
        }
        if (s->nets_pass_bad) {
            return "bad password";
        }
    }
    if (s->relay_bad || s->n_names > NET_RELAY_NAMES_MAX || s->n_addrs > NET_RELAY_ADDRS_MAX
        || s->port < 1 || s->port > 65535) {
        return "bad relay";
    }
    if (s->f[PF_REMOTE] == F_OK && !netcfg_url_split(out->remote_url, s->url_len, NULL)) {
        return "bad remote_url";
    }
    memcpy(out->identity, s->ident, NET_IDENTITY_LEN + 1);
    memcpy(out->key_id, s->kid, NET_KEY_ID_LEN + 1);
    out->n_names = (uint8_t)s->n_names;
    out->n_addrs = (uint8_t)s->n_addrs;
    out->port = (uint16_t)s->port;
    out->has_remote = s->f[PF_REMOTE] == F_OK;
    if (!out->has_remote) {
        out->remote_url[0] = 0;
    }
    if (s->f[PF_NETWORKS] == F_OK) {
        out->n_nets = (uint8_t)s->n_nets;
    } else {
        out->nets[0] = s->top;
        out->nets[0].lan = true;
        out->n_nets = 1;
    }
    return NULL;
}

const char *netcfg_parse_pair(const uint8_t *text, size_t len, const char *identity, netcfg_t *out)
{
    netcfg_wipe(out, sizeof *out);
    if (len > NET_PAIR_MAX_BYTES) {
        return "too long";
    }
    pair_scan_t s;
    memset(&s, 0, sizeof s);
    jcur_t c = {.p = text, .end = text + len, .bad = false};
    if (jkind(&c) != JK_OBJ) {
        c.bad = true;
    } else {
        c.p++;
        if (!jeat(&c, '}')) {
            do {
                char key[KEY_BUF];
                size_t klen;
                if (!jkey(&c, key, &klen)) {
                    c.bad = true;
                    break;
                }
                int field = pair_key(key, klen);
                if (field < 0) {
                    jskip(&c, 1);
                } else {
                    pair_value(&c, &s, out, field);
                }
            } while (!c.bad && jeat(&c, ','));
            if (!c.bad && !jeat(&c, '}')) {
                c.bad = true;
            }
        }
        jws(&c);
        if (c.p != c.end) {
            c.bad = true;
        }
    }
    const char *err = c.bad ? "bad json" : pair_verdict(&s, identity, out);
    netcfg_wipe(&s, sizeof s);
    if (err) {
        netcfg_wipe(out, sizeof *out);
    }
    return err;
}

bool netcfg_parse_remote(const uint8_t *text, size_t len, char url[NET_URL_MAX + 1])
{
    url[0] = 0;
    if (len > NET_REMOTE_MAX_BYTES) {
        return false;
    }
    jcur_t c = {.p = text, .end = text + len, .bad = false};
    uint8_t got = F_ABSENT;
    size_t ulen = 0;
    if (jkind(&c) != JK_OBJ) {
        return false;
    }
    c.p++;
    if (!jeat(&c, '}')) {
        do {
            char key[KEY_BUF];
            size_t klen;
            if (!jkey(&c, key, &klen)) {
                c.bad = true;
                break;
            }
            if (!key_is(key, klen, "url")) {
                jskip(&c, 1);
            } else if (got != F_ABSENT || jkind(&c) != JK_STR) {
                got = F_BAD;
                jskip(&c, 1);
            } else {
                ulen = jstring(&c, url, NET_URL_MAX + 1);
                got = F_OK;
            }
        } while (!c.bad && jeat(&c, ','));
        if (!c.bad && !jeat(&c, '}')) {
            c.bad = true;
        }
    }
    jws(&c);
    bool ok = !c.bad && c.p == c.end && got == F_OK && netcfg_url_split(url, ulen, NULL);
    if (!ok) {
        url[0] = 0;
    }
    return ok;
}

void netcfg_wire_identity(const netcfg_t *cfg, char out[NET_WIRE_IDENTITY_LEN + 1])
{
    memcpy(out, cfg->identity, NET_IDENTITY_LEN);
    out[NET_IDENTITY_LEN] = '-';
    memcpy(out + NET_IDENTITY_LEN + 1, cfg->key_id, NET_KEY_ID_LEN + 1);
}

size_t netcfg_result_json(const char *err, char *buf, size_t cap)
{
    int n = err ? snprintf(buf, cap, "{\"ok\":false,\"error\":\"%s\"}", err) : snprintf(buf, cap, "{\"ok\":true}");
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

size_t netcfg_status_json(const net_status_t *st, char *buf, size_t cap)
{
    static const char *const wifi[] = {"off", "connecting", "connected", "failed"};
    static const char *const relay[] = {"none", "connecting", "connected", "auth_failed", "unreachable", "captive", "tls_failed"};
    int w = st->wifi >= 0 && st->wifi <= NET_WIFI_FAILED ? st->wifi : NET_WIFI_FAILED;
    int r = st->relay >= 0 && st->relay <= NET_RELAY_TLS_FAILED ? st->relay : NET_RELAY_NONE;
    int n;
    if (w == NET_WIFI_CONNECTED) {
        n = snprintf(buf, cap, "{\"wifi\":\"%s\",\"ip\":\"%u.%u.%u.%u\",\"rssi\":%d,\"relay\":\"%s\"}", wifi[w],
                     (unsigned)st->ip[0], (unsigned)st->ip[1], (unsigned)st->ip[2], (unsigned)st->ip[3], st->rssi, relay[r]);
    } else {
        n = snprintf(buf, cap, "{\"wifi\":\"%s\",\"relay\":\"%s\"}", wifi[w], relay[r]);
    }
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}
