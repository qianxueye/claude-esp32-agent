#include "net.h"
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include "app_config.h"
#include "driver/usb_serial_jtag.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_tls_errors.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linkstate.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/ssl.h"
#include "netcfg.h"
#include "netplan.h"
#include "netstore.h"
#include "pairrec.h"
#include "netwrite.h"
#include "proto.h"
#include "transport.h"
#include "wsmap.h"

/* Task model, locks and the generation counter: docs/network.md. */

static const char *TAG = "net";

enum { EV_CONFIG = 1, EV_REMOTE, EV_SCAN_DONE, EV_DISCONNECTED, EV_GOT_IP, EV_LOST_IP };
typedef struct {
    uint8_t ev;
    uint32_t ip; /* network byte order, EV_GOT_IP only */
} net_ev_t;

enum { SESSION_RAN = -1 };

static char s_identity[NET_IDENTITY_LEN + 1];
static SemaphoreHandle_t s_cfg_mtx, s_tx_mtx;
static QueueHandle_t s_evq;
static TaskHandle_t s_task;

/* Shared configuration, under s_cfg_mtx. */
static netcfg_t s_cfg;
static bool s_paired;
static uint32_t s_pair_id;
static char s_remote[NET_URL_MAX + 1];
static bool s_has_remote;
static char s_stored[NET_URL_MAX + 1]; /* the remote URL in effect in flash */
static bool s_has_stored;
static _Atomic uint32_t s_gen, s_remote_gen;

/* Handler scratch; handlers run one at a time under the dispatch lock. */
static uint8_t s_pair_text[NET_PAIR_MAX_BYTES + 1];
static netcfg_t s_pair_tmp;
static uint8_t s_remote_text[NET_REMOTE_MAX_BYTES + 1];
static char s_url_tmp[NET_URL_MAX + 1];

/* Status published by the task. */
static portMUX_TYPE s_st_mux = portMUX_INITIALIZER_UNLOCKED;
static net_status_t s_status = {.wifi = NET_WIFI_OFF, .relay = NET_RELAY_NONE};

/* Frame queue: producers take s_tx_mtx, the task is the only consumer. */
static netwrite_t s_tx;
static bool s_tx_ok;
static _Atomic uint32_t s_tx_lock_drops;
static _Atomic bool s_activated;

/* Owned by the network task. */
static netcfg_t t_cfg;
static bool t_paired, t_has_remote, t_joined, t_wifi_broken, t_backoff_cut, t_usb_was;
static char t_remote[NET_URL_MAX + 1];
static uint32_t t_gen, t_remote_gen;
static int t_net;
static netplan_t s_np;
static bool s_wifi_ready, s_wifi_started;
static wifi_ap_record_t s_ap;
static np_ap_t s_picks[WIFI_NETWORKS_MAX];
static mbedtls_ctr_drbg_context s_drbg;
static bool s_drbg_seeded;
static mbedtls_ssl_context s_ssl;
static mbedtls_ssl_config s_conf;
static uint8_t *s_ws_tx, *s_rx_store;
static uint8_t s_rbuf[NET_READ_CHUNK_BYTES];

static void post(uint8_t ev, uint32_t ip)
{
    net_ev_t e = {.ev = ev, .ip = ip};
    if (s_evq) {
        xQueueSend(s_evq, &e, 0);
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        post(EV_SCAN_DONE, 0);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        post(EV_DISCONNECTED, 0);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        post(EV_GOT_IP, ((ip_event_got_ip_t *)data)->ip_info.ip.addr);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        post(EV_LOST_IP, 0);
    }
}

static bool stop_now(void)
{
    return atomic_load(&s_gen) != t_gen;
}

static void publish(void)
{
    net_status_t st = s_np.st;
    portENTER_CRITICAL(&s_st_mux);
    bool changed = np_status_differs(&s_status, &st);
    s_status = st;
    portEXIT_CRITICAL(&s_st_mux);
    if (changed) {
        char json[NET_STATUS_JSON_BYTES];
        size_t n = netcfg_status_json(&st, json, sizeof json);
        if (n) {
            transport_send(T_WIFI_STATUS, json, n);
        }
    }
}

static void poll_usb(void)
{
    bool c = usb_serial_jtag_is_connected();
    if (transport_link_active() == LINK_USB && t_usb_was && !c) {
        np_usb_lost(&s_np);
        t_backoff_cut = true;
    }
    t_usb_was = c;
}

static size_t tries_now(np_try_t tries[NP_TRIES_MAX])
{
    return np_tries(&t_cfg, t_net, t_has_remote, tries);
}

static void take_remote(void)
{
    uint32_t g = atomic_load(&s_remote_gen);
    if (g == t_remote_gen) {
        return;
    }
    t_remote_gen = g;
    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
    memcpy(t_remote, s_remote, sizeof t_remote);
    t_has_remote = s_has_remote;
    xSemaphoreGive(s_cfg_mtx);
    if (t_joined) {
        np_try_t tries[NP_TRIES_MAX];
        np_remote_changed(&s_np, tries_now(tries));
        t_backoff_cut = true;
        publish();
    }
}

static void wifi_lost(void)
{
    if (t_joined) {
        t_joined = false;
        np_wifi_down(&s_np);
        publish();
    }
}

/* Waits up to ms for one event and applies what it changes; returns the event or 0. */
static int pump_events(uint32_t ms, net_ev_t *out)
{
    net_ev_t e = {0};
    bool got = xQueueReceive(s_evq, &e, pdMS_TO_TICKS(ms)) == pdTRUE;
    poll_usb();
    take_remote();
    if (got && (e.ev == EV_DISCONNECTED || e.ev == EV_LOST_IP)) {
        wifi_lost();
    } else if (got && e.ev == EV_GOT_IP && t_joined) {
        wifi_ap_record_t ap;
        np_wifi_ip(&s_np, (const uint8_t *)&e.ip, esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : s_np.st.rssi);
        publish();
    }
    if (out) {
        *out = e;
    }
    return got ? e.ev : 0;
}

/* Waits ms; a backoff reset shortens the wait to NET_BACKOFF_MIN_MS. Returns early on a config change. */
static void wait_backoff(uint32_t ms, bool need_join)
{
    uint32_t end = now_ms() + ms;
    t_backoff_cut = false;
    while (!stop_now() && (!need_join || t_joined)) {
        if (t_backoff_cut) {
            t_backoff_cut = false;
            uint32_t cut = now_ms() + NET_BACKOFF_MIN_MS;
            if ((int32_t)(cut - end) < 0) {
                end = cut;
            }
        }
        int32_t left = (int32_t)(end - now_ms());
        if (left <= 0) {
            return;
        }
        pump_events(left < 100 ? (uint32_t)left : 100, NULL);
    }
}

static void drain_events(void)
{
    net_ev_t e;
    while (xQueueReceive(s_evq, &e, 0) == pdTRUE) {
    }
}

/* Initialises netif, driver and handlers once; a failed step is retried at the next PAIR. */
static bool wifi_ensure_init(void)
{
    static esp_netif_t *netif;
    static bool driver, handlers;
    esp_err_t e = esp_netif_init();
    if (e == ESP_OK) {
        e = esp_event_loop_create_default();
        if (e == ESP_ERR_INVALID_STATE) {
            e = ESP_OK;
        }
    }
    if (e == ESP_OK && !netif) {
        esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_WIFI_STA();
        esp_netif_t *n = esp_netif_new(&ncfg);
        e = n ? esp_netif_attach_wifi_station(n) : ESP_ERR_NO_MEM;
        if (e == ESP_OK) {
            netif = n;
        } else if (n) {
            esp_netif_destroy(n);
        }
    }
    if (e == ESP_OK && !driver) {
        wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
        e = esp_wifi_init(&wcfg);
        if (e == ESP_OK) {
            e = esp_wifi_set_default_wifi_sta_handlers();
        }
        if (e == ESP_OK) {
            e = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        }
        if (e == ESP_OK) {
            e = esp_wifi_set_mode(WIFI_MODE_STA);
        }
        if (e == ESP_OK) {
            driver = true;
        } else {
            esp_wifi_deinit();
        }
    }
    if (e == ESP_OK && !handlers) {
        e = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL);
        if (e == ESP_OK) {
            e = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL);
        }
        handlers = e == ESP_OK;
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed (%s)", esp_err_to_name(e));
        return false;
    }
    s_wifi_ready = true;
    return true;
}

static void wifi_stop(void)
{
    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_ready) {
        wifi_config_t z;
        memset(&z, 0, sizeof z);
        esp_wifi_set_config(WIFI_IF_STA, &z);
    }
    t_joined = false;
    drain_events();
}

/* Takes the shared configuration after PAIR, FORGET or boot and restarts Wi-Fi for it. */
static void reconfigure(void)
{
    t_gen = atomic_load(&s_gen);
    wifi_stop();
    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
    t_cfg = s_cfg;
    t_paired = s_paired;
    memcpy(t_remote, s_remote, sizeof t_remote);
    t_has_remote = s_has_remote;
    t_remote_gen = atomic_load(&s_remote_gen);
    xSemaphoreGive(s_cfg_mtx);
    np_reset(&s_np, t_paired);
    t_wifi_broken = false;
    if (t_paired) {
        esp_err_t e = ESP_FAIL;
        if (wifi_ensure_init()) {
            e = esp_wifi_start();
        }
        if (e == ESP_OK) {
            s_wifi_started = true;
        } else {
            ESP_LOGE(TAG, "Wi-Fi start failed (%s)", esp_err_to_name(e));
            t_wifi_broken = true;
            s_np.st.wifi = NET_WIFI_FAILED;
        }
        ESP_LOGI(TAG, "paired: %u networks, relay names %u, addresses %u, remote %s", (unsigned)t_cfg.n_nets,
                 (unsigned)t_cfg.n_names, (unsigned)t_cfg.n_addrs, t_has_remote ? t_remote : "none");
    }
    publish();
}

/* Waits for one of two events; returns the one that arrived, or 0 at timeout or a config change. */
static int wait_for(uint8_t a, uint8_t b, uint32_t ms, net_ev_t *got)
{
    uint32_t end = now_ms() + ms;
    while (!stop_now()) {
        int32_t left = (int32_t)(end - now_ms());
        if (left <= 0) {
            return 0;
        }
        net_ev_t e;
        int ev = pump_events(left < 100 ? (uint32_t)left : 100, &e);
        if (ev && (ev == a || ev == b)) {
            *got = e;
            return ev;
        }
    }
    return 0;
}

static void join(void)
{
    np_assoc_start(&s_np);
    publish();
    drain_events();
    wifi_scan_config_t sc;
    memset(&sc, 0, sizeof sc);
    net_ev_t e;
    if (esp_wifi_scan_start(&sc, false) != ESP_OK || !wait_for(EV_SCAN_DONE, EV_SCAN_DONE, NET_SCAN_TIMEOUT_MS, &e)) {
        esp_wifi_scan_stop();
        esp_wifi_clear_ap_list();
        if (!stop_now()) {
            uint32_t d = np_assoc_failed(&s_np);
            publish();
            wait_backoff(d, false);
        }
        return;
    }
    uint16_t n = 0;
    if (esp_wifi_scan_get_ap_num(&n) != ESP_OK) {
        n = 0;
    }
    size_t kept = 0;
    for (uint16_t i = 0; i < n && esp_wifi_scan_get_ap_record(&s_ap) == ESP_OK; i++) {
        np_ap_t a;
        a.ssid_len = (uint8_t)strnlen((const char *)s_ap.ssid, NET_SSID_MAX);
        memcpy(a.ssid, s_ap.ssid, a.ssid_len);
        a.rssi = s_ap.rssi;
        kept = np_scan_keep(&t_cfg, s_picks, kept, &a);
    }
    esp_wifi_clear_ap_list();
    int idx = np_pick(&t_cfg, s_picks, kept);
    if (idx < 0) {
        ESP_LOGI(TAG, "scan: no saved network among %u", (unsigned)n);
        uint32_t d = np_scan_none(&s_np);
        publish();
        wait_backoff(d, false);
        return;
    }
    const netcfg_net_t *net = &t_cfg.nets[idx];
    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);
    memcpy(wc.sta.ssid, net->ssid, strlen(net->ssid));
    memcpy(wc.sta.password, net->password, strlen(net->password));
    wc.sta.threshold.authmode = net->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    mbedtls_platform_zeroize(&wc, sizeof wc);
    ESP_LOGI(TAG, "joining %s", net->ssid);
    drain_events();
    int ev = err == ESP_OK && esp_wifi_connect() == ESP_OK ? wait_for(EV_GOT_IP, EV_DISCONNECTED, NET_ASSOC_TIMEOUT_MS, &e) : 0;
    if (ev == EV_GOT_IP) {
        t_joined = true;
        t_net = idx;
        wifi_ap_record_t ap;
        int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
        np_try_t tries[NP_TRIES_MAX];
        np_wifi_up(&s_np, (const uint8_t *)&e.ip, rssi, tries_now(tries));
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        publish();
        return;
    }
    if (stop_now()) {
        return;
    }
    esp_wifi_disconnect();
    uint32_t d = np_assoc_failed(&s_np);
    publish();
    wait_backoff(d, false);
}

static int hw_entropy(void *ctx, unsigned char *out, size_t len)
{
    esp_fill_random(out, len);
    return 0;
}

static int inner_write(void *ctx, const uint8_t *buf, size_t len)
{
    int r = mbedtls_ssl_write(ctx, buf, len);
    if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) {
        return NW_WANT_WRITE;
    }
    return r;
}

static void on_net_frame(int kind, const proto_frame_t *f, uint32_t *last_rx)
{
    *last_rx = now_ms();
    transport_net_frame(kind, f);
}

/* Runs the inner TLS session over a connected BIO; returns SESSION_RAN or the NP_FAIL stage. */
/* Seeds the DRBG on the first attempt and reseeds it on every later one.
 * Called only while associated: the RNG is a true RNG only with the radio on (docs/network.md "Inner TLS"). */
static bool drbg_fresh(void)
{
    static const char pers[] = "clawd-net";
    int r = s_drbg_seeded ? mbedtls_ctr_drbg_reseed(&s_drbg, NULL, 0)
                          : mbedtls_ctr_drbg_seed(&s_drbg, hw_entropy, NULL, (const unsigned char *)pers, sizeof pers - 1);
    s_drbg_seeded = s_drbg_seeded || r == 0;
    return r == 0;
}

static int run_session(int kind, mbedtls_ssl_send_t *snd, mbedtls_ssl_recv_t *rcv, void *bio)
{
    if (!t_joined || !drbg_fresh()) {
        return NP_FAIL_INNER;
    }
    static const int suites[] = {MBEDTLS_TLS_ECDHE_PSK_WITH_CHACHA20_POLY1305_SHA256, 0};
    mbedtls_ssl_init(&s_ssl);
    mbedtls_ssl_config_init(&s_conf);
    int stage = NP_FAIL_INNER;
    int r = mbedtls_ssl_config_defaults(&s_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (r == 0) {
        mbedtls_ssl_conf_min_tls_version(&s_conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&s_conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_ciphersuites(&s_conf, suites);
#if defined(MBEDTLS_SSL_SESSION_TICKETS)
        mbedtls_ssl_conf_session_tickets(&s_conf, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
#endif
#if defined(MBEDTLS_SSL_RENEGOTIATION)
        mbedtls_ssl_conf_renegotiation(&s_conf, MBEDTLS_SSL_RENEGOTIATION_DISABLED);
#endif
        mbedtls_ssl_conf_rng(&s_conf, mbedtls_ctr_drbg_random, &s_drbg);
        r = mbedtls_ssl_conf_psk(&s_conf, t_cfg.psk, NET_PSK_BYTES, (const unsigned char *)t_cfg.identity, NET_IDENTITY_LEN);
    }
    if (r == 0) {
        r = mbedtls_ssl_setup(&s_ssl, &s_conf);
    }
    if (r == 0) {
        mbedtls_ssl_set_bio(&s_ssl, bio, snd, rcv, NULL);
        uint32_t end = now_ms() + NET_INNER_HANDSHAKE_MS;
        do {
            r = mbedtls_ssl_handshake(&s_ssl);
            if (r == MBEDTLS_ERR_SSL_WANT_WRITE) {
                vTaskDelay(1);
            }
        } while ((r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) && !stop_now() && t_joined
                 && (int32_t)(end - now_ms()) > 0);
        if (r == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE || r == MBEDTLS_ERR_SSL_INVALID_MAC) {
            stage = NP_FAIL_AUTH;
        }
        if (r != 0) {
            ESP_LOGW(TAG, "relay handshake failed (-0x%04x)", (unsigned)-r);
        }
    }
    if (r != 0) {
        mbedtls_ssl_free(&s_ssl);
        mbedtls_ssl_config_free(&s_conf);
        return stage;
    }

    np_session_up(&s_np);
    publish();
    esp_wifi_set_ps(WIFI_PS_NONE);
    nw_discard(&s_tx);
    atomic_store(&s_activated, false);
    proto_parser_t parser;
    proto_parser_init(&parser, s_rx_store, PROTO_MAX_PAYLOAD + FRAME_OVERHEAD + NET_READ_CHUNK_BYTES);
    proto_parser_set_stream(&parser, true);
    uint32_t last_rx = now_ms();
    const char *why = "silence";
    ESP_LOGI(TAG, "relay session open (%s)", kind == LINK_TUNNEL ? "tunnel" : "wifi");
    for (;;) {
        pump_events(0, NULL);
        if (stop_now() || !t_joined) {
            why = "config or Wi-Fi change";
            break;
        }
        int n = mbedtls_ssl_read(&s_ssl, s_rbuf, sizeof s_rbuf);
        if (n > 0) {
            size_t off = 0;
            do {
                off += proto_parser_feed(&parser, s_rbuf + off, (size_t)n - off);
                proto_frame_t f;
                while (proto_parser_next(&parser, now_ms(), &f)) {
                    on_net_frame(kind, &f, &last_rx);
                    proto_parser_release(&parser, &f);
                }
            } while (off < (size_t)n);
        } else if (n != MBEDTLS_ERR_SSL_WANT_READ && n != MBEDTLS_ERR_SSL_WANT_WRITE) {
            why = "closed by the relay or the link";
            break;
        }
        if (atomic_exchange(&s_activated, false)) {
            np_session_active(&s_np);
        }
        if (nw_pump(&s_tx, inner_write, &s_ssl) == NW_FAILED) {
            why = "write failed";
            break;
        }
        if (now_ms() - last_rx >= PROTO_HOST_SILENCE_TIMEOUT_MS) {
            break;
        }
    }
    ESP_LOGI(TAG, "relay session closed: %s", why);
    mbedtls_ssl_close_notify(&s_ssl);
    transport_net_ended();
    nw_discard(&s_tx);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    mbedtls_ssl_free(&s_ssl);
    mbedtls_ssl_config_free(&s_conf);
    return SESSION_RAN;
}

static int lan_send(void *ctx, const unsigned char *buf, size_t len)
{
    int n = send(*(int *)ctx, buf, len, MSG_DONTWAIT);
    if (n > 0) {
        return n;
    }
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int lan_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(fd, &rs);
    struct timeval tv = {.tv_sec = 0, .tv_usec = NET_POLL_MS * 1000};
    int s = select(fd + 1, &rs, NULL, NULL, &tv);
    if (s == 0) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    int n = s > 0 ? recv(fd, buf, len, MSG_DONTWAIT) : -1;
    if (n >= 0) {
        return n;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
}

/* Connects to one LAN relay address within LAN_CONNECT_TIMEOUT_MS; returns the socket or -1 with *stage set. */
static int lan_connect(const np_try_t *t, int *stage)
{
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(t_cfg.port)};
    if (t->kind == NP_TRY_NAME) {
        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
        if (getaddrinfo(t_cfg.names[t->index], NULL, &hints, &res) != 0 || !res) {
            *stage = NP_FAIL_NAME;
            return -1;
        }
        sa.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    } else {
        memcpy(&sa.sin_addr.s_addr, t_cfg.addrs[t->index], 4);
    }
    *stage = NP_FAIL_TCP;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    uint32_t end = now_ms() + t->timeout_ms;
    for (;;) {
        fd_set ws;
        FD_ZERO(&ws);
        FD_SET(fd, &ws);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100 * 1000};
        int s = select(fd + 1, NULL, &ws, NULL, &tv);
        if (s > 0) {
            int err = 0;
            socklen_t el = sizeof err;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
            if (err == 0) {
                break;
            }
        }
        if (s < 0 || s > 0 || stop_now() || (int32_t)(end - now_ms()) <= 0) {
            close(fd);
            return -1;
        }
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

static int ws_send(void *ctx, const unsigned char *buf, size_t len)
{
    size_t n = len < NET_WS_TX_BYTES ? len : NET_WS_TX_BYTES;
    memcpy(s_ws_tx, buf, n);
    int poll = esp_transport_poll_write(ctx, NET_POLL_MS);
    int sent = poll > 0 ? esp_transport_ws_send_raw(ctx, (ws_transport_opcodes_t)(WS_TRANSPORT_OPCODES_BINARY | WS_TRANSPORT_OPCODES_FIN),
                                                    (const char *)s_ws_tx, (int)n, NET_WS_WRITE_TIMEOUT_MS)
                          : 0;
    switch (wsmap_write(poll, sent, (int)n)) {
    case WSW_SENT: return (int)n;
    case WSW_WANT_WRITE: return MBEDTLS_ERR_SSL_WANT_WRITE;
    default: return MBEDTLS_ERR_NET_SEND_FAILED;
    }
}

static int ws_recv(void *ctx, unsigned char *buf, size_t len)
{
    int poll = esp_transport_poll_read(ctx, NET_POLL_MS);
    if (poll == 0) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    int n = poll > 0 ? esp_transport_read(ctx, (char *)buf, (int)len, NET_WS_READ_TIMEOUT_MS) : -1;
    switch (wsmap_read(n, (int)esp_transport_ws_get_read_opcode(ctx), esp_transport_ws_get_read_payload_len(ctx))) {
    case WSR_DATA: return n;
    case WSR_WANT_READ: return MBEDTLS_ERR_SSL_WANT_READ;
    case WSR_EOS: return 0;
    default: return MBEDTLS_ERR_NET_RECV_FAILED;
    }
}

static int tunnel_fail_stage(esp_transport_handle_t ssl, int status)
{
    if (status > 0 && status != 101) {
        return NP_FAIL_UPGRADE;
    }
    esp_tls_last_error_t *e = esp_transport_get_error_handle(ssl);
    if (!e) {
        return NP_FAIL_OUTER;
    }
    if (e->last_error == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) {
        return NP_FAIL_NAME;
    }
    if (e->esp_tls_flags != 0 || e->esp_tls_error_code == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        return NP_FAIL_CERT;
    }
    if (e->last_error == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST) {
        return NP_FAIL_TCP;
    }
    return status < 0 ? NP_FAIL_UPGRADE : NP_FAIL_OUTER;
}

static int try_tunnel(void)
{
    netcfg_url_t u;
    if (!netcfg_url_split(t_remote, strlen(t_remote), &u)) {
        return NP_FAIL_OUTER;
    }
    esp_transport_handle_t ssl = esp_transport_ssl_init();
    esp_transport_handle_t ws = ssl ? esp_transport_ws_init(ssl) : NULL;
    if (!ws) {
        if (ssl) {
            esp_transport_destroy(ssl);
        }
        return NP_FAIL_OUTER;
    }
    esp_transport_ssl_crt_bundle_attach(ssl, esp_crt_bundle_attach);
    esp_transport_ws_config_t wcfg = {.ws_path = u.path, .propagate_control_frames = false};
    esp_transport_ws_set_config(ws, &wcfg);
    int r = esp_transport_connect(ws, u.host, u.port, REMOTE_STEP_TIMEOUT_MS);
    int status = esp_transport_ws_get_upgrade_request_status(ws);
    int stage;
    if (r != 0 || status != 101) {
        stage = tunnel_fail_stage(ssl, status);
        ESP_LOGW(TAG, "tunnel %s failed: status %d", u.host, status);
    } else {
        int fd = esp_transport_get_socket(ws);
        int one = 1;
        if (fd >= 0) {
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        }
        stage = run_session(LINK_TUNNEL, ws_send, ws_recv, ws);
    }
    esp_transport_close(ws);
    esp_transport_destroy(ws);
    esp_transport_destroy(ssl);
    return stage;
}

static int try_one(const np_try_t *t)
{
    if (t->kind == NP_TRY_REMOTE) {
        return try_tunnel();
    }
    int stage;
    int fd = lan_connect(t, &stage);
    if (fd < 0) {
        return stage;
    }
    stage = run_session(LINK_WIFI, lan_send, lan_recv, &fd);
    close(fd);
    return stage;
}

static void relay_round(void)
{
    np_try_t tries[NP_TRIES_MAX];
    size_t n = tries_now(tries);
    if (n == 0) {
        wait_backoff(NET_BACKOFF_MIN_MS, true);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (stop_now() || !t_joined) {
            return;
        }
        int r = try_one(&tries[i]);
        if (r == SESSION_RAN) {
            uint32_t d = np_session_down(&s_np);
            publish();
            wait_backoff(d, true);
            return;
        }
        np_try_failed(&s_np, np_fail_status(tries[i].kind, r));
        take_remote();
    }
    if (stop_now() || !t_joined) {
        return;
    }
    uint32_t d = np_round_failed(&s_np);
    publish();
    wait_backoff(d, true);
}

static void net_task(void *arg)
{
    mbedtls_ctr_drbg_init(&s_drbg);
    t_gen = atomic_load(&s_gen) - 1;
    for (;;) {
        if (stop_now()) {
            reconfigure();
        }
        if (!t_paired || t_wifi_broken) {
            pump_events(1000, NULL);
        } else if (!t_joined) {
            join();
        } else {
            relay_round();
        }
    }
}

void net_init(void)
{
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY) != ESP_OK) {
        memset(mac, 0, sizeof mac);
    }
    netcfg_identity(mac, s_identity);
    esp_log_level_set("transport_ws", ESP_LOG_NONE);
    s_cfg_mtx = xSemaphoreCreateMutex();
    s_tx_mtx = xSemaphoreCreateMutex();
    s_evq = xQueueCreate(NET_EVENT_QUEUE_DEPTH, sizeof(net_ev_t));
    if (!s_cfg_mtx || !s_evq) {
        ESP_LOGE(TAG, "network module not started: out of memory");
        return;
    }
    uint32_t pair_id, remote_id;
    size_t len = netstore_init() ? netstore_load(s_pair_text, NET_PAIR_MAX_BYTES, &pair_id, s_url_tmp, &remote_id) : 0;
    if (len) {
        const char *err = netcfg_parse_pair(s_pair_text, len, s_identity, &s_cfg);
        if (err) {
            mbedtls_platform_zeroize(&s_cfg, sizeof s_cfg);
            ESP_LOGW(TAG, "saved pairing refused (%s): unpaired", err);
        } else {
            bool url_ok = s_url_tmp[0] && netcfg_url_split(s_url_tmp, strlen(s_url_tmp), NULL);
            const char *remote = pr_effective_remote(pair_id, &s_cfg, remote_id, url_ok ? s_url_tmp : NULL);
            s_paired = true;
            s_pair_id = pair_id;
            s_has_remote = remote != NULL;
            if (remote) {
                memmove(s_remote, remote, strlen(remote) + 1);
                memcpy(s_stored, s_remote, sizeof s_stored);
            }
            s_has_stored = s_has_remote;
        }
    }
    mbedtls_platform_zeroize(s_pair_text, sizeof s_pair_text);
}

void net_start(void)
{
    uint8_t *ring = heap_caps_malloc(NET_TX_RING_BYTES, MALLOC_CAP_SPIRAM);
    s_ws_tx = heap_caps_malloc(NET_WS_TX_BYTES, MALLOC_CAP_SPIRAM);
    s_rx_store = heap_caps_malloc(PROTO_MAX_PAYLOAD + FRAME_OVERHEAD + NET_READ_CHUNK_BYTES, MALLOC_CAP_SPIRAM);
    bool ok = s_cfg_mtx && s_evq && s_tx_mtx && ring && s_ws_tx && s_rx_store && nw_init(&s_tx, ring, NET_TX_RING_BYTES);
    if (!ok || xTaskCreate(net_task, "net", STACK_NET, NULL, PRIO_NET, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "network task not started: out of memory");
        return;
    }
    s_tx_ok = true;
}

bool net_paired(void)
{
    if (!s_cfg_mtx) {
        return false;
    }
    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
    bool p = s_paired;
    xSemaphoreGive(s_cfg_mtx);
    return p;
}

const char *net_pair(const uint8_t *payload, size_t len)
{
    if (len > NET_PAIR_MAX_BYTES) {
        return "too long";
    }
    if (!s_cfg_mtx || !s_evq) {
        return "storage error";
    }
    memcpy(s_pair_text, payload, len);
    const char *err = netcfg_parse_pair(s_pair_text, len, s_identity, &s_pair_tmp);
    uint32_t id = 0;
    if (!err && !netstore_save_pair(s_pair_text, len, &id)) {
        err = "storage error";
    }
    if (!err) {
        xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
        s_cfg = s_pair_tmp;
        s_paired = true;
        s_pair_id = id;
        memcpy(s_remote, s_pair_tmp.remote_url, sizeof s_remote);
        s_has_remote = s_pair_tmp.has_remote;
        memcpy(s_stored, s_remote, sizeof s_stored);
        s_has_stored = s_has_remote;
        xSemaphoreGive(s_cfg_mtx);
        atomic_fetch_add(&s_gen, 1);
        post(EV_CONFIG, 0);
    }
    ESP_LOGI(TAG, "PAIR %s: %u networks", err ? err : "stored", (unsigned)s_pair_tmp.n_nets);
    mbedtls_platform_zeroize(s_pair_text, sizeof s_pair_text);
    mbedtls_platform_zeroize(&s_pair_tmp, sizeof s_pair_tmp);
    return err;
}

const char *net_forget(void)
{
    bool ok = netstore_forget();
    if (!s_cfg_mtx) {
        return ok ? NULL : "storage error";
    }
    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
    mbedtls_platform_zeroize(&s_cfg, sizeof s_cfg);
    s_paired = false;
    s_pair_id = 0;
    s_remote[0] = 0;
    s_has_remote = false;
    s_stored[0] = 0;
    s_has_stored = false;
    xSemaphoreGive(s_cfg_mtx);
    atomic_fetch_add(&s_gen, 1);
    post(EV_CONFIG, 0);
    ESP_LOGI(TAG, "FORGET %s", ok ? "done" : "storage error");
    return ok ? NULL : "storage error";
}

void net_set_remote(const uint8_t *payload, size_t len)
{
    static char failed[NET_URL_MAX + 1]; /* the URL whose save failed last, logged once */
    if (!s_cfg_mtx) {
        return;
    }
    if (len > NET_REMOTE_MAX_BYTES) {
        ESP_LOGW(TAG, "SET_REMOTE ignored: %u bytes", (unsigned)len);
        return;
    }
    memcpy(s_remote_text, payload, len);
    bool ok = netcfg_parse_remote(s_remote_text, len, s_url_tmp);
    mbedtls_platform_zeroize(s_remote_text, sizeof s_remote_text);
    if (!ok) {
        ESP_LOGW(TAG, "SET_REMOTE ignored: invalid");
        return;
    }
    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
    bool paired = s_paired;
    pr_remote_plan_t plan = pr_remote_plan(s_has_stored ? s_stored : NULL, s_has_remote ? s_remote : NULL, s_url_tmp);
    uint32_t pair_id = s_pair_id, gen = atomic_load(&s_gen);
    xSemaphoreGive(s_cfg_mtx);
    if (!paired || (!plan.save && !plan.apply)) {
        return;
    }
    int saved = plan.save ? netstore_save_remote(pair_id, &s_gen, gen, s_url_tmp) : NS_SAVED;
    if (saved == NS_STALE) {
        return;
    }
    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
    bool current = atomic_load(&s_gen) == gen;
    if (current && plan.save && saved == NS_SAVED) {
        memcpy(s_stored, s_url_tmp, sizeof s_stored);
        s_has_stored = true;
    }
    if (current && plan.apply) {
        memcpy(s_remote, s_url_tmp, sizeof s_remote);
        s_has_remote = true;
    }
    xSemaphoreGive(s_cfg_mtx);
    if (!current) {
        return;
    }
    if (saved == NS_FAILED && strcmp(failed, s_url_tmp) != 0) {
        memcpy(failed, s_url_tmp, sizeof failed);
        ESP_LOGW(TAG, "remote URL %s not saved; used until restart", s_url_tmp);
    } else if (saved == NS_SAVED) {
        failed[0] = 0;
    }
    if (plan.apply) {
        atomic_fetch_add(&s_remote_gen, 1);
        post(EV_REMOTE, 0);
        ESP_LOGI(TAG, "remote URL %s", s_url_tmp);
    }
}

size_t net_status_json(char *buf, size_t cap)
{
    net_status_t st = {.wifi = NET_WIFI_OFF, .relay = NET_RELAY_NONE};
    if (net_paired() && !s_task) {
        st.wifi = NET_WIFI_FAILED;
    } else if (net_paired()) {
        portENTER_CRITICAL(&s_st_mux);
        st = s_status;
        portEXIT_CRITICAL(&s_st_mux);
    }
    return netcfg_status_json(&st, buf, cap);
}

bool net_send(uint8_t type, const void *payload, size_t len, bool reliable)
{
    if (!s_tx_ok) {
        return false;
    }
    bool running = xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
    if (!reliable || !running) {
        if (xSemaphoreTake(s_tx_mtx, running ? pdMS_TO_TICKS(NET_TX_LOCK_WAIT_MS) : 0) != pdTRUE) {
            atomic_fetch_add(&s_tx_lock_drops, 1);
            return false;
        }
        bool ok = nw_put(&s_tx, type, payload, len);
        xSemaphoreGive(s_tx_mtx);
        return ok;
    }
    uint32_t start = now_ms();
    for (;;) {
        xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
        bool ok = nw_offer(&s_tx, type, payload, len);
        bool late = !ok && now_ms() - start >= TX_SNAPSHOT_WRITE_TIMEOUT_MS;
        if (late) {
            s_tx.dropped++;
        }
        xSemaphoreGive(s_tx_mtx);
        if (ok || late) {
            return ok;
        }
        vTaskDelay(1);
    }
}

void net_session_activated(void)
{
    atomic_store(&s_activated, true);
}

uint32_t net_stack_hwm(void)
{
    return s_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0;
}

uint32_t net_tx_dropped(void)
{
    return s_tx.dropped + atomic_load(&s_tx_lock_drops);
}
