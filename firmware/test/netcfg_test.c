#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netcfg.h"

static int s_fail, s_checks;
#define CHECK(name, cond) check((name), (cond), __LINE__)

static void check(const char *name, int ok, int line)
{
    s_checks++;
    if (!ok) {
        s_fail++;
        printf("FAIL netcfg_test.c:%d %s\n", line, name);
    }
}

#define ID "clawd-AABBCCDDEE01"
#define PSK64 "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"

static netcfg_t cfg;
static char J[2048];

/* PAIR as pairing.py pair_payload encodes it (json.dumps, ensure_ascii=False, compact separators), four networks. */
static const char FULL4[] = "{\"ssid\":\"HomeNet\",\"password\":\"correct horse\",\"psk_identity\":\"clawd-AABBCCDDEE01\",\"psk\":\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\",\"relay_names\":[\"relay.local\"],\"relay_addrs\":[\"10.0.0.2\",\"192.168.1.20\"],\"relay_port\":47801,\"networks\":[{\"ssid\":\"HomeNet\",\"password\":\"correct horse\",\"lan\":true},{\"ssid\":\"Caf\xC3\xA9 5G\",\"password\":\"pass\\\"word\\\\x\",\"lan\":false},{\"ssid\":\"Phone \xF0\x9F\x90\xB1\",\"password\":\"12345678\",\"lan\":false},{\"ssid\":\"Office\",\"password\":\"\",\"lan\":false}]}";
/* The same encoder with a fifth network. */
static const char FIVE[] = "{\"ssid\":\"HomeNet\",\"password\":\"correct horse\",\"psk_identity\":\"clawd-AABBCCDDEE01\",\"psk\":\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\",\"relay_names\":[\"relay.local\"],\"relay_addrs\":[\"10.0.0.2\",\"192.168.1.20\"],\"relay_port\":47801,\"networks\":[{\"ssid\":\"HomeNet\",\"password\":\"correct horse\",\"lan\":true},{\"ssid\":\"Caf\xC3\xA9 5G\",\"password\":\"pass\\\"word\\\\x\",\"lan\":false},{\"ssid\":\"Phone \xF0\x9F\x90\xB1\",\"password\":\"12345678\",\"lan\":false},{\"ssid\":\"Office\",\"password\":\"\",\"lan\":false},{\"ssid\":\"Extra\",\"password\":\"87654321\",\"lan\":false}]}";
/* SET_REMOTE as link.py encodes it (json.dumps with default separators). */
static const char REMOTE[] = "{\"url\": \"wss://example.trycloudflare.com/clawd/v1\"}";

static const char *parse(const char *json, size_t len)
{
    return netcfg_parse_pair((const uint8_t *)json, len, ID, &cfg);
}

static const char *parse_s(const char *json)
{
    return parse(json, strlen(json));
}

/* Hand-written PAIR with the seven required members; ssid and pass are JSON string literals. */
static const char *mk(const char *ssid, const char *pass, const char *ident, const char *psk, const char *tail)
{
    snprintf(J, sizeof J,
             "{\"ssid\":%s,\"password\":%s,\"psk_identity\":\"%s\",\"psk\":\"%s\",\"relay_names\":[\"relay.local\"],"
             "\"relay_addrs\":[\"10.0.0.2\"],\"relay_port\":47801%s}",
             ssid, pass, ident, psk, tail);
    return J;
}

static const char *mk_relay(const char *names, const char *addrs, const char *port)
{
    snprintf(J, sizeof J,
             "{\"ssid\":\"HomeNet\",\"password\":\"correct horse\",\"psk_identity\":\"" ID "\",\"psk\":\"" PSK64
             "\",\"relay_names\":%s,\"relay_addrs\":%s,\"relay_port\":%s}",
             names, addrs, port);
    return J;
}

static int is_err(const char *got, const char *want)
{
    if (!want || !got) {
        return got == want;
    }
    return strcmp(got, want) == 0;
}

static void expect(const char *name, const char *json, const char *want)
{
    const char *got = parse_s(json);
    if (!is_err(got, want)) {
        printf("  %s: got %s, want %s\n", name, got ? got : "accepted", want ? want : "accepted");
    }
    CHECK(name, is_err(got, want));
}

static void test_minimal_and_full(void)
{
    const char *min = mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64, "");
    expect("minimal PAIR accepted", min, NULL);
    CHECK("minimal: one network from the top-level pair, lan true",
          cfg.n_nets == 1 && strcmp(cfg.nets[0].ssid, "HomeNet") == 0 && strcmp(cfg.nets[0].password, "correct horse") == 0
              && cfg.nets[0].lan);
    CHECK("minimal: psk bytes 0x00..0x1f", cfg.psk[0] == 0x00 && cfg.psk[17] == 0x11 && cfg.psk[31] == 0x1f);
    CHECK("minimal: relay 'relay.local' 10.0.0.2:47801",
          cfg.n_names == 1 && strcmp(cfg.names[0], "relay.local") == 0 && cfg.n_addrs == 1 && cfg.addrs[0][0] == 10
              && cfg.addrs[0][3] == 2 && cfg.port == 47801 && !cfg.has_remote && strcmp(cfg.identity, ID) == 0);

    expect("host PAIR with 4 networks accepted", FULL4, NULL);
    CHECK("4 networks: count and lan flags",
          cfg.n_nets == 4 && cfg.nets[0].lan && !cfg.nets[1].lan && !cfg.nets[2].lan && !cfg.nets[3].lan);
    CHECK("4 networks: raw UTF-8 SSID 'Cafe 5G' with e-acute", strcmp(cfg.nets[1].ssid, "Caf\xC3\xA9 5G") == 0);
    CHECK("4 networks: escaped quote and backslash in password", strcmp(cfg.nets[1].password, "pass\"word\\x") == 0);
    CHECK("4 networks: 4-byte character in SSID", strcmp(cfg.nets[2].ssid, "Phone \xF0\x9F\x90\xB1") == 0);
    CHECK("4 networks: open network has empty password", cfg.nets[3].password[0] == 0 && strcmp(cfg.nets[3].ssid, "Office") == 0);
    CHECK("4 networks: second address 192.168.1.20",
          cfg.n_addrs == 2 && cfg.addrs[1][0] == 192 && cfg.addrs[1][1] == 168 && cfg.addrs[1][2] == 1 && cfg.addrs[1][3] == 20);
    expect("host PAIR with 5 networks refused", FIVE, "too many networks");
    CHECK("refused PAIR leaves the output wiped", cfg.n_nets == 0 && cfg.psk[17] == 0 && cfg.nets[0].ssid[0] == 0);
}

static void test_psk_and_identity(void)
{
    expect("psk 63 hex digits refused", mk("\"HomeNet\"", "\"correct horse\"", ID, &PSK64[1], ""), "bad psk");
    expect("psk 64 hex digits accepted", mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64, ""), NULL);
    expect("psk 65 hex digits refused", mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64 "0", ""), "bad psk");
    char g[65];
    memcpy(g, PSK64, 65);
    g[40] = 'g';
    expect("psk 64 digits with one g refused", mk("\"HomeNet\"", "\"correct horse\"", ID, g, ""), "bad psk");
    expect("psk uppercase hex accepted", mk("\"HomeNet\"", "\"correct horse\"", ID,
                                            "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", ""), NULL);
    CHECK("uppercase psk decodes to 0x1f", cfg.psk[31] == 0x1f);

    expect("identity uppercase accepted", mk("\"HomeNet\"", "\"correct horse\"", "clawd-AABBCCDDEE01", PSK64, ""), NULL);
    expect("identity lowercase refused", mk("\"HomeNet\"", "\"correct horse\"", "clawd-aabbccddee01", PSK64, ""),
           "identity mismatch");
    expect("identity of another MAC refused", mk("\"HomeNet\"", "\"correct horse\"", "clawd-AABBCCDDEE02", PSK64, ""),
           "identity mismatch");
    char id[NET_IDENTITY_LEN + 1];
    netcfg_identity((const uint8_t[6]){0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01}, id);
    CHECK("identity of AA:BB:CC:DD:EE:01 is clawd-AABBCCDDEE01", strcmp(id, "clawd-AABBCCDDEE01") == 0);
}

static void test_ssid(void)
{
    expect("SSID 32 bytes accepted", mk("\"ABCDEFGHIJKLMNOPQRSTUVWXYZ012345\"", "\"correct horse\"", ID, PSK64, ""), NULL);
    CHECK("SSID 32 bytes stored whole", strlen(cfg.nets[0].ssid) == 32);
    expect("SSID 33 bytes refused", mk("\"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456\"", "\"correct horse\"", ID, PSK64, ""), "bad ssid");
    expect("SSID 16 x e-acute (32 bytes) accepted",
           mk("\"\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\"",
              "\"correct horse\"", ID, PSK64, ""), NULL);
    expect("SSID empty refused", mk("\"\"", "\"correct horse\"", ID, PSK64, ""), "bad ssid");
    expect("raw UTF-8 SSID accepted", mk("\"Caf\xC3\xA9\"", "\"correct horse\"", ID, PSK64, ""), NULL);
    CHECK("raw UTF-8 SSID kept as received", strcmp(cfg.nets[0].ssid, "Caf\xC3\xA9") == 0);
    expect("invalid UTF-8: lone continuation 0x80", mk("\"Caf\x80\"", "\"correct horse\"", ID, PSK64, ""), "bad json");
    expect("invalid UTF-8: overlong C0 AF", mk("\"a\xC0\xAF\"", "\"correct horse\"", ID, PSK64, ""), "bad json");
    expect("invalid UTF-8: overlong E0 80 AF", mk("\"a\xE0\x80\xAF\"", "\"correct horse\"", ID, PSK64, ""), "bad json");
    expect("invalid UTF-8: encoded surrogate ED A0 80", mk("\"a\xED\xA0\x80\"", "\"correct horse\"", ID, PSK64, ""), "bad json");
    expect("invalid UTF-8: truncated E2 82", mk("\"a\xE2\x82\"", "\"correct horse\"", ID, PSK64, ""), "bad json");
    expect("invalid UTF-8: above U+10FFFF F4 90 80 80", mk("\"a\xF4\x90\x80\x80\"", "\"correct horse\"", ID, PSK64, ""),
           "bad json");
    expect("invalid UTF-8: F5 lead byte", mk("\"a\xF5\x80\x80\x80\"", "\"correct horse\"", ID, PSK64, ""), "bad json");
    expect("valid UTF-8: U+10FFFF F4 8F BF BF", mk("\"a\xF4\x8F\xBF\xBF\"", "\"correct horse\"", ID, PSK64, ""), NULL);
}

static void test_password(void)
{
    expect("password 0 bytes (open) accepted", mk("\"HomeNet\"", "\"\"", ID, PSK64, ""), NULL);
    expect("password 7 bytes refused", mk("\"HomeNet\"", "\"1234567\"", ID, PSK64, ""), "bad password");
    expect("password 8 bytes accepted", mk("\"HomeNet\"", "\"12345678\"", ID, PSK64, ""), NULL);
    expect("password 63 bytes accepted",
           mk("\"HomeNet\"", "\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijk\"", ID, PSK64, ""), NULL);
    expect("password 64 hex accepted", mk("\"HomeNet\"", "\"" PSK64 "\"", ID, PSK64, ""), NULL);
    CHECK("password 64 hex stored whole", strcmp(cfg.nets[0].password, PSK64) == 0);
    expect("password 64 non-hex refused",
           mk("\"HomeNet\"", "\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijkl\"", ID, PSK64, ""),
           "bad password");
    expect("password 65 bytes refused", mk("\"HomeNet\"", "\"" PSK64 "0\"", ID, PSK64, ""), "bad password");
}

static void test_strings(void)
{
    expect("escape \\u00e9 accepted", mk("\"Caf\\u00e9\"", "\"correct horse\"", ID, PSK64, ""), NULL);
    CHECK("\\u00e9 decodes to C3 A9", strcmp(cfg.nets[0].ssid, "Caf\xC3\xA9") == 0);
    expect("surrogate pair accepted", mk("\"x\\ud83d\\ude00\"", "\"correct horse\"", ID, PSK64, ""), NULL);
    CHECK("\\ud83d\\ude00 decodes to F0 9F 98 80", strcmp(cfg.nets[0].ssid, "x\xF0\x9F\x98\x80") == 0);
    expect("lone high surrogate refused", mk("\"x\\ud83d\"", "\"correct horse\"", ID, PSK64, ""), "bad json");
    expect("high surrogate then a non-surrogate refused", mk("\"x\\ud83d\\u0041\"", "\"correct horse\"", ID, PSK64, ""), "bad json");
    expect("lone low surrogate refused", mk("\"x\\ude00\"", "\"correct horse\"", ID, PSK64, ""), "bad json");
    expect("\\u0000 refused", mk("\"x\\u0000y\"", "\"correct horse\"", ID, PSK64, ""), "bad json");
    expect("raw control byte 0x01 refused", mk("\"x\x01y\"", "\"correct horse\"", ID, PSK64, ""), "bad json");
    expect("raw control byte 0x1f refused", mk("\"x\x1fy\"", "\"correct horse\"", ID, PSK64, ""), "bad json");
    expect("escaped newline accepted", mk("\"a\\nb\"", "\"correct horse\"", ID, PSK64, ""), NULL);
    CHECK("escaped newline decodes to 0x0A", strcmp(cfg.nets[0].ssid, "a\nb") == 0);
    expect("unknown escape \\x refused", mk("\"a\\xb\"", "\"correct horse\"", ID, PSK64, ""), "bad json");
    expect("all short escapes accepted", mk("\"\\\"\\\\\\/\\b\\f\\r\\t\"", "\"correct horse\"", ID, PSK64, ""), NULL);
    CHECK("short escapes decode", strcmp(cfg.nets[0].ssid, "\"\\/\b\f\r\t") == 0);
}

static void test_structure(void)
{
    expect("duplicate psk refused", mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64, ",\"psk\":\"" PSK64 "\""),
           "bad field psk");
    const char *unk = mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64,
                         ",\"x\":[[1,2],{\"a\":null},-2.5e3,true],\"access_id\":\"id-1\",\"access_secret\":\"S3cr3tValue\"");
    expect("unknown keys with nested array and access_secret accepted", unk, NULL);
    int found = 0;
    for (size_t i = 0; i + 4 <= sizeof cfg; i++) {
        found |= memcmp((const uint8_t *)&cfg + i, "cr3t", 4) == 0;
    }
    CHECK("access_secret bytes appear nowhere in the parsed config", !found);
    expect("depth 3 accepted", mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64, ",\"x\":[[1]]"), NULL);
    expect("depth 4 refused", mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64, ",\"x\":[[[1]]]"), "bad json");
    expect("depth 4 object inside a network refused",
           mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64, ",\"networks\":[{\"ssid\":\"A\",\"password\":\"\",\"lan\":true,\"x\":[1]}]"),
           "bad json");
    char trail[sizeof J + 8];
    snprintf(trail, sizeof trail, "%sx", mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64, ""));
    expect("trailing x refused", trail, "bad json");
    snprintf(trail, sizeof trail, "%s \r\n\t", mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64, ""));
    expect("trailing whitespace accepted", trail, NULL);
    expect("whitespace between tokens accepted",
           "{ \"ssid\" : \"HomeNet\" ,\n\t\"password\":\"correct horse\",\r\n\"psk_identity\" :\"" ID "\", \"psk\":\"" PSK64
           "\",\"relay_names\": [ \"relay.local\" ] ,\"relay_addrs\":[ ],\"relay_port\" : 47801 }",
           NULL);
    expect("top-level array refused", "[1]", "bad json");
    expect("empty payload refused", "", "bad json");
    expect("trailing comma refused", mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64, ","), "bad json");
    expect("number with leading zero refused", mk_relay("[]", "[]", "047801"), "bad json");
    expect("missing relay_port refused",
           "{\"ssid\":\"HomeNet\",\"password\":\"correct horse\",\"psk_identity\":\"" ID "\",\"psk\":\"" PSK64
           "\",\"relay_names\":[],\"relay_addrs\":[]}",
           "bad field relay_port");
    expect("ssid as a number refused", mk("7", "\"correct horse\"", ID, PSK64, ""), "bad field ssid");
    expect("relay_port as a string refused", mk_relay("[]", "[]", "\"47801\""), "bad field relay_port");
    expect("relay_port negative refused", mk_relay("[]", "[]", "-1"), "bad field relay_port");
    expect("relay_port with a fraction refused", mk_relay("[]", "[]", "1.5"), "bad field relay_port");
    expect("relay_names holding a number refused", mk_relay("[1]", "[]", "47801"), "bad field relay_names");
    expect("remote_url as null refused", mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64, ",\"remote_url\":null"),
           "bad field remote_url");
}

static void test_size(void)
{
    static char big[1100];
    const char *base = mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64, "");
    size_t n = strlen(base) - 1;
    memcpy(big, base, n);
    n += (size_t)snprintf(big + n, sizeof big - n, ",\"pad\":\"");
    size_t pad = 1023 - n - 2;
    memset(big + n, 'p', pad);
    memcpy(big + n + pad, "\"}", 3);
    CHECK("padded payload is 1023 bytes", strlen(big) == 1023);
    expect("1023-byte payload accepted", big, NULL);
    memmove(big + n + 1, big + n, strlen(big + n) + 1);
    CHECK("padded payload is 1024 bytes", strlen(big) == 1024);
    expect("1024-byte payload refused", big, "too long");
}

static void test_relay(void)
{
    expect("relay_port 0 refused", mk_relay("[]", "[]", "0"), "bad relay");
    expect("relay_port 1 accepted", mk_relay("[]", "[]", "1"), NULL);
    CHECK("relay_port 1 stored", cfg.port == 1);
    expect("relay_port 65535 accepted", mk_relay("[]", "[]", "65535"), NULL);
    CHECK("relay_port 65535 stored", cfg.port == 65535);
    expect("relay_port 65536 refused", mk_relay("[]", "[]", "65536"), "bad relay");
    expect("relay_port 99999999999 refused", mk_relay("[]", "[]", "99999999999"), "bad relay");
    expect("4 relay names accepted", mk_relay("[\"a.local\",\"b.local\",\"c.local\",\"d.local\"]", "[]", "47801"), NULL);
    expect("5 relay names refused", mk_relay("[\"a.local\",\"b.local\",\"c.local\",\"d.local\",\"e.local\"]", "[]", "47801"),
           "bad relay");
    expect("relay name 63 bytes accepted",
           mk_relay("[\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcde.local\"]", "[]", "47801"), NULL);
    expect("relay name 64 bytes refused",
           mk_relay("[\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdef.local\"]", "[]", "47801"), "bad relay");
    expect("empty relay name refused", mk_relay("[\"\"]", "[]", "47801"), "bad relay");
    expect("8 relay addresses accepted",
           mk_relay("[]", "[\"10.0.0.1\",\"10.0.0.2\",\"10.0.0.3\",\"10.0.0.4\",\"10.0.0.5\",\"10.0.0.6\",\"10.0.0.7\",\"10.0.0.8\"]",
                    "47801"), NULL);
    CHECK("8 addresses stored", cfg.n_addrs == 8 && cfg.addrs[7][3] == 8);
    expect("9 relay addresses refused",
           mk_relay("[]", "[\"10.0.0.1\",\"10.0.0.2\",\"10.0.0.3\",\"10.0.0.4\",\"10.0.0.5\",\"10.0.0.6\",\"10.0.0.7\",\"10.0.0.8\",\"10.0.0.9\"]",
                    "47801"), "bad relay");
    expect("address 255.255.255.255 accepted", mk_relay("[]", "[\"255.255.255.255\"]", "47801"), NULL);
    expect("address 10.0.0.256 refused", mk_relay("[]", "[\"10.0.0.256\"]", "47801"), "bad relay");
    expect("address with leading zero 10.0.0.01 refused", mk_relay("[]", "[\"10.0.0.01\"]", "47801"), "bad relay");
    expect("address 10.0.0 refused", mk_relay("[]", "[\"10.0.0\"]", "47801"), "bad relay");
    expect("address 10.0.0.1.2 refused", mk_relay("[]", "[\"10.0.0.1.2\"]", "47801"), "bad relay");
    expect("host name as address refused", mk_relay("[]", "[\"relay.local\"]", "47801"), "bad relay");
}

static void test_networks(void)
{
    expect("networks replace the top-level pair",
           mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64, ",\"networks\":[{\"ssid\":\"Other\",\"password\":\"12345678\",\"lan\":false}]"),
           NULL);
    CHECK("only the listed network is kept",
          cfg.n_nets == 1 && strcmp(cfg.nets[0].ssid, "Other") == 0 && !cfg.nets[0].lan && strcmp(cfg.nets[0].password, "12345678") == 0);
    expect("empty networks refused", mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64, ",\"networks\":[]"), "bad field networks");
    expect("network without lan refused",
           mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64, ",\"networks\":[{\"ssid\":\"A\",\"password\":\"\"}]"), "bad field networks");
    expect("network entry that is a string refused", mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64, ",\"networks\":[\"A\"]"),
           "bad field networks");
    expect("network with a 33-byte SSID refused",
           mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64,
              ",\"networks\":[{\"ssid\":\"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456\",\"password\":\"\",\"lan\":true}]"), "bad ssid");
    expect("network with a 7-byte password refused",
           mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64, ",\"networks\":[{\"ssid\":\"A\",\"password\":\"1234567\",\"lan\":true}]"),
           "bad password");
    expect("network with a duplicate ssid refused",
           mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64,
              ",\"networks\":[{\"ssid\":\"A\",\"ssid\":\"B\",\"password\":\"\",\"lan\":true}]"), "bad field networks");
    expect("PAIR with remote_url accepted",
           mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64, ",\"remote_url\":\"wss://example.trycloudflare.com/clawd/v1\""), NULL);
    CHECK("remote_url stored", cfg.has_remote && strcmp(cfg.remote_url, "wss://example.trycloudflare.com/clawd/v1") == 0);
    expect("PAIR with ws:// remote_url refused",
           mk("\"HomeNet\"", "\"correct horse\"", ID, PSK64, ",\"remote_url\":\"ws://example.trycloudflare.com/clawd/v1\""),
           "bad remote_url");
}

static int remote_ok(const char *json)
{
    char url[NET_URL_MAX + 1];
    return netcfg_parse_remote((const uint8_t *)json, strlen(json), url);
}

static int url_ok(const char *url)
{
    return netcfg_url_split(url, strlen(url), NULL);
}

static void test_remote(void)
{
    char url[NET_URL_MAX + 1];
    CHECK("SET_REMOTE from link.py accepted", netcfg_parse_remote((const uint8_t *)REMOTE, strlen(REMOTE), url));
    CHECK("SET_REMOTE URL stored", strcmp(url, "wss://example.trycloudflare.com/clawd/v1") == 0);
    CHECK("SET_REMOTE with access_id and access_secret accepted",
          remote_ok("{\"url\":\"wss://a.example/clawd/v1\",\"access_id\":\"i\",\"access_secret\":\"s\"}"));
    CHECK("SET_REMOTE ws:// refused", !remote_ok("{\"url\": \"ws://example.trycloudflare.com/clawd/v1\"}"));
    CHECK("SET_REMOTE empty host refused", !remote_ok("{\"url\": \"wss:///clawd/v1\"}"));
    CHECK("SET_REMOTE 'wss://a b/' refused", !remote_ok("{\"url\": \"wss://a b/\"}"));
    CHECK("SET_REMOTE without url refused", !remote_ok("{\"access_id\":\"i\"}"));
    CHECK("SET_REMOTE duplicate url refused", !remote_ok("{\"url\":\"wss://a.b/\",\"url\":\"wss://a.b/\"}"));
    CHECK("SET_REMOTE trailing x refused", !remote_ok("{\"url\":\"wss://a.b/\"}x"));
    char u[300] = "wss://";
    memset(u + 6, 'a', 245);
    memcpy(u + 251, ".b/x", 5);
    CHECK("URL of 255 bytes accepted", strlen(u) == 255 && url_ok(u));
    memcpy(u + 251, ".b/xy", 6);
    CHECK("URL of 256 bytes refused", strlen(u) == 256 && !url_ok(u));
    char js[600];
    snprintf(js, sizeof js, "{\"url\":\"%s\"}", u);
    CHECK("SET_REMOTE with a 256-byte URL refused", !remote_ok(js));
    u[255] = 0;
    snprintf(js, sizeof js, "{\"url\":\"%s\"}", u);
    CHECK("SET_REMOTE with a 255-byte URL accepted", remote_ok(js));
    CHECK("URL path with an escaped space refused", !remote_ok("{\"url\":\"wss://a.b/x\\u0020y\"}"));
    CHECK("URL path with escaped CR LF refused", !remote_ok("{\"url\":\"wss://a.b/x\\r\\nHost: c\"}"));
    CHECK("URL path with a raw non-ASCII byte refused", !url_ok("wss://a.b/x\xC3\xA9"));
    CHECK("URL without a path refused", !url_ok("wss://a.b"));
    CHECK("URL with an empty port refused", !url_ok("wss://a.b:/x"));
    CHECK("URL with port 0 refused", !url_ok("wss://a.b:0/x"));
    CHECK("URL with port 65536 refused", !url_ok("wss://a.b:65536/x"));
    CHECK("URL with port 1 accepted", url_ok("wss://a.b:1/x"));
    CHECK("URL with port 65535 accepted", url_ok("wss://a.b:65535/x"));
    CHECK("URL with user info refused", !url_ok("wss://u@a.b/x"));
    CHECK("URL with uppercase scheme refused", !url_ok("WSS://a.b/x"));
    netcfg_url_t parts;
    const char *v = "wss://relay-1.example:8443/clawd/v1";
    CHECK("URL split host, port and path",
          netcfg_url_split(v, strlen(v), &parts) && strcmp(parts.host, "relay-1.example") == 0 && parts.port == 8443
              && strcmp(parts.path, "/clawd/v1") == 0);
    v = "wss://example.trycloudflare.com/clawd/v1";
    CHECK("URL split default port 443", netcfg_url_split(v, strlen(v), &parts) && parts.port == 443);
    static char big[600];
    int n = snprintf(big, sizeof big, "{\"url\":\"wss://a.b/\",\"access_secret\":\"");
    memset(big + n, 's', 511 - (size_t)n - 2);
    memcpy(big + 511 - 2, "\"}", 3);
    CHECK("SET_REMOTE of 511 bytes accepted", strlen(big) == 511 && remote_ok(big));
    memmove(big + n + 1, big + n, strlen(big + n) + 1);
    CHECK("SET_REMOTE of 512 bytes refused", strlen(big) == 512 && !remote_ok(big));
}

static void test_json_out(void)
{
    char buf[128];
    net_status_t st = {.wifi = NET_WIFI_OFF, .relay = NET_RELAY_NONE};
    size_t n = netcfg_status_json(&st, buf, sizeof buf);
    CHECK("unpaired status is exactly {\"wifi\":\"off\",\"relay\":\"none\"}",
          n == strlen(buf) && strcmp(buf, "{\"wifi\":\"off\",\"relay\":\"none\"}") == 0);
    st = (net_status_t){.wifi = NET_WIFI_CONNECTED, .relay = NET_RELAY_CAPTIVE, .ip = {10, 0, 0, 7}, .rssi = -55};
    netcfg_status_json(&st, buf, sizeof buf);
    CHECK("connected status carries ip and rssi",
          strcmp(buf, "{\"wifi\":\"connected\",\"ip\":\"10.0.0.7\",\"rssi\":-55,\"relay\":\"captive\"}") == 0);
    st = (net_status_t){.wifi = NET_WIFI_FAILED, .relay = NET_RELAY_NONE, .ip = {10, 0, 0, 7}, .rssi = -55};
    netcfg_status_json(&st, buf, sizeof buf);
    CHECK("failed status has no ip or rssi", strcmp(buf, "{\"wifi\":\"failed\",\"relay\":\"none\"}") == 0);
    st = (net_status_t){.wifi = NET_WIFI_CONNECTED, .relay = NET_RELAY_TLS_FAILED, .ip = {255, 255, 255, 255}, .rssi = -100};
    CHECK("status too long for its buffer gives 0", netcfg_status_json(&st, buf, 20) == 0);
    CHECK("PAIR_RESULT ok", netcfg_result_json(NULL, buf, sizeof buf) == 11 && strcmp(buf, "{\"ok\":true}") == 0);
    netcfg_result_json("bad psk", buf, sizeof buf);
    CHECK("PAIR_RESULT error", strcmp(buf, "{\"ok\":false,\"error\":\"bad psk\"}") == 0);
}

int main(void)
{
    test_minimal_and_full();
    test_psk_and_identity();
    test_ssid();
    test_password();
    test_strings();
    test_structure();
    test_size();
    test_relay();
    test_networks();
    test_remote();
    test_json_out();
    printf("netcfg: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
