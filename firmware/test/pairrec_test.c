#include <stdio.h>
#include <string.h>
#include "pairrec.h"

static int s_fail, s_checks;
#define CHECK(name, cond) check((name), (cond), __LINE__)

static void check(const char *name, int ok, int line)
{
    s_checks++;
    if (!ok) {
        s_fail++;
        printf("FAIL pairrec_test.c:%d %s\n", line, name);
    }
}

static const char PAIR_TEXT[] = "{\"ssid\":\"Home\",\"password\":\"password1\"}";
static const char URL_PAIR[] = "wss://pair.example.com/clawd/v1";
static const char URL_SET[] = "wss://set.example.com/clawd/v1";

static void test_round_trip(void)
{
    uint8_t blob[PR_PAIR_BLOB_MAX];
    size_t len = strlen(PAIR_TEXT);
    size_t n = pr_encode(7, PAIR_TEXT, len, NET_PAIR_MAX_BYTES, blob, sizeof blob);
    CHECK("encoded size is header + text", n == PR_HEADER_BYTES + len);
    uint32_t id = 0;
    const uint8_t *data = NULL;
    size_t dlen = 0;
    CHECK("round trip decodes", pr_decode(blob, n, NET_PAIR_MAX_BYTES, &id, &data, &dlen));
    CHECK("round trip keeps id 7 and the text", id == 7 && dlen == len && memcmp(data, PAIR_TEXT, len) == 0);
    CHECK("truncated by one byte is refused", !pr_decode(blob, n - 1, NET_PAIR_MAX_BYTES, &id, &data, &dlen));
    CHECK("one extra byte is refused", !pr_decode(blob, n + 1, NET_PAIR_MAX_BYTES, &id, &data, &dlen));
    CHECK("shorter than the header is refused", !pr_decode(blob, PR_HEADER_BYTES - 1, NET_PAIR_MAX_BYTES, &id, &data, &dlen));
    blob[0] = PR_VERSION + 1;
    CHECK("another version is refused", !pr_decode(blob, n, NET_PAIR_MAX_BYTES, &id, &data, &dlen));
    blob[0] = PR_VERSION;
    CHECK("a record over the reader's maximum is refused", !pr_decode(blob, n, len - 1, &id, &data, &dlen));
    memset(blob + 1, 0, 4);
    CHECK("pairing id 0 is refused", !pr_decode(blob, n, NET_PAIR_MAX_BYTES, &id, &data, &dlen));

    static uint8_t big[NET_PAIR_MAX_BYTES + 1];
    CHECK("1024 bytes of PAIR text are not encoded", pr_encode(1, big, sizeof big, NET_PAIR_MAX_BYTES, blob, sizeof blob) == 0);
    CHECK("1023 bytes of PAIR text are encoded", pr_encode(1, big, NET_PAIR_MAX_BYTES, NET_PAIR_MAX_BYTES, blob, sizeof blob) == PR_PAIR_BLOB_MAX);
    CHECK("id 0 is not encoded", pr_encode(0, PAIR_TEXT, len, NET_PAIR_MAX_BYTES, blob, sizeof blob) == 0);
    CHECK("too small an output is not written", pr_encode(1, PAIR_TEXT, len, NET_PAIR_MAX_BYTES, blob, PR_HEADER_BYTES + len - 1) == 0);
}

static void test_ids_and_remote(void)
{
    netcfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    CHECK("a PAIR without remote_url and no SET_REMOTE has no remote URL", pr_effective_remote(1, &cfg, 0, NULL) == NULL);
    CHECK("a SET_REMOTE record of the same pairing applies", pr_effective_remote(1, &cfg, 1, URL_SET) == URL_SET);
    CHECK("a SET_REMOTE record of another pairing is ignored", pr_effective_remote(2, &cfg, 1, URL_SET) == NULL);

    cfg.has_remote = true;
    strcpy(cfg.remote_url, URL_PAIR);
    CHECK("the PAIR's remote_url applies without a SET_REMOTE record", pr_effective_remote(1, &cfg, 0, NULL) == cfg.remote_url);
    CHECK("SET_REMOTE of the same pairing replaces the PAIR's remote_url", pr_effective_remote(1, &cfg, 1, URL_SET) == URL_SET);

    uint32_t next = pr_next_id(1, 1);
    CHECK("the next PAIR after pairing 1 and its SET_REMOTE gets id 2", next == 2);
    CHECK("after that PAIR the old SET_REMOTE URL stops applying", pr_effective_remote(next, &cfg, 1, URL_SET) == cfg.remote_url);
    CHECK("an empty store starts at id 1", pr_next_id(0, 0) == 1);
    CHECK("a leftover SET_REMOTE id is passed too", pr_next_id(0, 5) == 6);
    CHECK("the id never wraps to 0", pr_next_id(UINT32_MAX, 0) == 1);
}

static void test_remote_plan(void)
{
    pr_remote_plan_t p = pr_remote_plan(NULL, NULL, URL_SET);
    CHECK("a first URL is saved and used", p.save && p.apply);
    /* The save failed: the URL is in use but the stored URL is still none. */
    p = pr_remote_plan(NULL, URL_SET, URL_SET);
    CHECK("after a failed save the same URL tries the save again", p.save && !p.apply);
    p = pr_remote_plan(URL_SET, URL_SET, URL_SET);
    CHECK("after a successful save the same URL does nothing", !p.save && !p.apply);
    p = pr_remote_plan(URL_PAIR, URL_PAIR, URL_SET);
    CHECK("a changed URL is saved and used", p.save && p.apply);
    p = pr_remote_plan(URL_SET, URL_PAIR, URL_SET);
    CHECK("a URL already stored but not in use is used without a save", !p.save && p.apply);
}

static void test_save_recheck(void)
{
    CHECK("same pairing and generation may save", pr_save_current(3, 3, 10, 10));
    CHECK("a newer PAIR drops the save", !pr_save_current(4, 3, 10, 10));
    CHECK("FORGET drops the save", !pr_save_current(0, 3, 11, 10));
    CHECK("a generation change drops the save", !pr_save_current(3, 3, 11, 10));
    CHECK("an unpaired plan never saves", !pr_save_current(0, 0, 10, 10));
}

static void test_boot_action(void)
{
    CHECK("init ok: use the partition", pr_boot_action(0) == PR_BOOT_OK);
    CHECK("no free pages (0x110d): erase and init again", pr_boot_action(0x110d) == PR_BOOT_REFORMAT);
    CHECK("new NVS version (0x1110): erase and init again", pr_boot_action(0x1110) == PR_BOOT_REFORMAT);
    CHECK("out of memory (0x101): unusable, nothing erased", pr_boot_action(0x101) == PR_BOOT_UNUSABLE);
    CHECK("flash operation failed (0x6001): unusable, nothing erased", pr_boot_action(0x6001) == PR_BOOT_UNUSABLE);
    CHECK("partition not found (0x105): unusable", pr_boot_action(0x105) == PR_BOOT_UNUSABLE);
}

static void test_write_outcome(void)
{
    static const uint8_t NEW[] = "new text", OLD[] = "old text", OTHER[] = "odd text";
    pr_view_t want = {true, 5, NEW, sizeof NEW}, prev = {true, 4, OLD, sizeof OLD}, none = {false, 0, NULL, 0};
    pr_view_t got_new = want, got_old = prev, got_other = {true, 5, OTHER, sizeof OTHER};
    CHECK("a write that succeeded is done", pr_write_outcome(true, false, none, want, prev) == PR_WRITE_DONE);
    CHECK("failed write, new record read back (remove failed): done", pr_write_outcome(false, true, got_new, want, prev) == PR_WRITE_DONE);
    CHECK("failed write, previous record read back: kept", pr_write_outcome(false, true, got_old, want, prev) == PR_WRITE_KEPT);
    CHECK("failed write, nothing stored and nothing before: kept", pr_write_outcome(false, true, none, want, none) == PR_WRITE_KEPT);
    CHECK("failed write, previous record gone: broken", pr_write_outcome(false, true, none, want, prev) == PR_WRITE_BROKEN);
    CHECK("failed write, new id with other bytes: broken", pr_write_outcome(false, true, got_other, want, prev) == PR_WRITE_BROKEN);
    CHECK("failed write, read-back failed: broken", pr_write_outcome(false, false, got_new, want, prev) == PR_WRITE_BROKEN);
    pr_view_t prev_hdr = {true, 4, NULL, sizeof OLD};
    CHECK("previous PAIR known by id and length only: kept", pr_write_outcome(false, true, got_old, want, prev_hdr) == PR_WRITE_KEPT);
    pr_view_t old_id_other_len = {true, 4, OLD, sizeof OLD - 1};
    CHECK("previous id with another length: broken", pr_write_outcome(false, true, old_id_other_len, want, prev_hdr) == PR_WRITE_BROKEN);
}

static void test_load_rule(void)
{
    CHECK("not found (0x1102) is absent", pr_read_class(0x1102, false) == PR_READ_ABSENT);
    CHECK("read and decoded is found", pr_read_class(0, true) == PR_READ_FOUND);
    CHECK("present but not decodable failed", pr_read_class(0, false) == PR_READ_FAILED);
    CHECK("a flash read error (0x6001) failed, not absent", pr_read_class(0x6001, false) == PR_READ_FAILED);
    CHECK("an invalid length (0x110c) failed", pr_read_class(0x110c, false) == PR_READ_FAILED);
    CHECK("both found: usable", pr_load_usable(PR_READ_FOUND, PR_READ_FOUND));
    CHECK("empty store: usable", pr_load_usable(PR_READ_ABSENT, PR_READ_ABSENT));
    CHECK("pair absent, remote left by an interrupted FORGET: usable", pr_load_usable(PR_READ_ABSENT, PR_READ_FOUND));
    CHECK("an unreadable remote makes the store unusable", !pr_load_usable(PR_READ_FOUND, PR_READ_FAILED));
    CHECK("an unreadable pair makes the store unusable", !pr_load_usable(PR_READ_FAILED, PR_READ_ABSENT));
    CHECK("with pair absent the next id still passes the remote id 1", pr_next_id(0, 1) == 2);
}

int main(void)
{
    test_load_rule();
    test_write_outcome();
    test_boot_action();
    test_round_trip();
    test_ids_and_remote();
    test_remote_plan();
    test_save_recheck();
    printf("pairrec: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail != 0;
}
