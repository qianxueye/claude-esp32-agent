import asyncio
import dataclasses
import json
import logging
import os
import stat
import sys

import pytest
from fake_device import FakeDevice, tls_connect
from test_relay import make, settle, until
from test_tunnel import TURN_INPUT
from test_tunnel import Harness as TunnelHarness
from test_wifi import IDENTITY, KEY, WifiHarness

from claude_voice import cli
from claude_voice.cloudflared import Backoff, Cloudflared, hostname_from_banner, metrics_hostname, remote_url
from claude_voice.config import load
from claude_voice.link import Link, TransportClosed
from claude_voice.protocol import State, T

FAKE_CLOUDFLARED = r'''
import http.server, json, os, signal, socketserver, sys, threading, time
here = os.path.dirname(os.path.abspath(__file__))
mode = json.load(open(os.path.join(here, "mode.json")))
runs_path = os.path.join(here, "runs")
n = int(open(runs_path).read()) + 1 if os.path.exists(runs_path) else 1
open(runs_path, "w").write(str(n))
with open(os.path.join(here, "argv.jsonl"), "a") as f:
    f.write(json.dumps({"argv": sys.argv[1:], "pid": os.getpid()}) + "\n")
if mode.get("ignore_term"):
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
lifetimes = mode.get("lifetimes", [])
life = lifetimes[n - 1] if n <= len(lifetimes) else None
if life == 0:
    sys.exit(1)
host = mode.get("hostname", "run{n}-quick.trycloudflare.com").format(n=n)
metrics = sys.argv[sys.argv.index("--metrics") + 1]
class Server(http.server.HTTPServer):
    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[0], self.server_address[1]
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/quicktunnel":
            self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
        body = json.dumps({"hostname": mode.get("metrics_value", host)}).encode()
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)
    def log_message(self, *a): pass
if mode.get("metrics", True):
    addr, port = metrics.rsplit(":", 1)
    srv = Server((addr, int(port)), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
if mode.get("banner", True):
    banner = mode.get("banner_host", host)
    print("INF +----------------------------------------------------+", file=sys.stderr, flush=True)
    print("INF |  Your quick Tunnel has been created! Visit it at:  |", file=sys.stderr, flush=True)
    print(f"INF |  https://{banner}  |", file=sys.stderr, flush=True)
start = time.monotonic()
crash = os.path.join(here, "crash")
while True:
    if os.path.exists(crash):
        os.unlink(crash)
        sys.exit(3)
    if life is not None and time.monotonic() - start > life:
        sys.exit(2)
    time.sleep(0.02)
'''


def fake_cloudflared(tmp_path, **mode):
    """Write the fake cloudflared and a shell wrapper (the interpreter path may contain spaces); returns the wrapper."""
    d = tmp_path / "cf"
    d.mkdir(exist_ok=True)
    (d / "fake.py").write_text(FAKE_CLOUDFLARED)
    (d / "mode.json").write_text(json.dumps(mode))
    wrapper = d / "cloudflared"
    wrapper.write_text(f'#!/bin/sh\nexec "{sys.executable}" -I "{d / "fake.py"}" "$@"\n')
    wrapper.chmod(wrapper.stat().st_mode | stat.S_IXUSR)
    return wrapper


def runs(tmp_path):
    f = tmp_path / "cf" / "argv.jsonl"
    return [json.loads(line) for line in f.read_text().splitlines()] if f.exists() else []


def cf_log(tmp_path):
    f = tmp_path / "logs" / "cloudflared.log"
    return f.read_text(errors="replace") if f.exists() else "(no cloudflared.log)"


def cf_cfg(cfg, tmp_path, binary, **over):
    base = dict(
        cloudflared_bin=str(binary), log_dir=str(tmp_path / "logs"), cloudflared_url_timeout_s=5.0,
        cloudflared_restart_min_s=0.05, cloudflared_restart_max_s=0.5,
    )
    return dataclasses.replace(cfg, **{**base, **over})


class Urls:
    def __init__(self):
        self.seen = []

    def __call__(self, url):
        self.seen.append(url)


def url(n):
    return f"wss://run{n}-quick.trycloudflare.com/clawd/v1"


async def supervised(tmp_path, on_url=None, cfg=None, port=47999, url_timeout=5.0, restart_max=0.5, **mode):
    from claude_voice.config import Config

    binary = fake_cloudflared(tmp_path, **mode)
    c = cf_cfg(
        cfg or Config(), tmp_path, binary, cloudflared_url_timeout_s=url_timeout, cloudflared_restart_max_s=restart_max
    )
    sup = Cloudflared(c, port, on_url or Urls(), stop_timeout_s=0.5)
    sup.start()
    return sup


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def test_hostname_patterns():
    assert metrics_hostname({"hostname": "abc-1.trycloudflare.com"}) == "abc-1.trycloudflare.com"
    assert metrics_hostname({"hostname": "https://abc-1.trycloudflare.com"}) == "abc-1.trycloudflare.com"
    assert metrics_hostname({"hostname": ""}) is None and metrics_hostname(["x"]) is None
    good = "INF |  https://abc-1.trycloudflare.com  |"
    assert hostname_from_banner(good) == "abc-1.trycloudflare.com"
    assert hostname_from_banner("https://abc-1.trycloudflare.com\n") == "abc-1.trycloudflare.com"
    for bad in (
        "https://abc.trycloudflare.com.evil.example |", "https://abc.trycloudflare.com/path |",
        "https://abc.trycloudflare.com:8443 |", "http://abc.trycloudflare.com |", "https://ABC.trycloudflare.com |",
        "https://a_b.trycloudflare.com |", "https://evil.example |",
    ):
        assert hostname_from_banner(bad) is None, bad
    assert remote_url("abc.trycloudflare.com") == "wss://abc.trycloudflare.com/clawd/v1"


def test_backoff_doubles_to_the_maximum_and_resets_after_a_long_run():
    b = Backoff(1.0, 8.0)
    assert [b.after(0.5) for _ in range(5)] == [1.0, 2.0, 4.0, 8.0, 8.0]
    assert b.after(8.0) == 8.0
    assert b.after(8.1) == 1.0 and b.after(0.1) == 2.0


async def test_hostname_from_metrics_with_the_exact_arguments(tmp_path):
    urls = Urls()
    sup = await supervised(tmp_path, urls, banner=False, metrics_value="https://run1-quick.trycloudflare.com")
    try:
        await until(lambda: urls.seen, timeout=10)
        assert urls.seen == [url(1)] and sup.url == url(1), cf_log(tmp_path)
        argv = runs(tmp_path)[0]["argv"]
        metrics = argv[-1]
        assert argv == ["tunnel", "--no-autoupdate", "--url", "http://127.0.0.1:47999", "--metrics", metrics]
        assert metrics.startswith("127.0.0.1:") and "--config" not in argv
    finally:
        await sup.close()


async def test_banner_fallback_when_metrics_is_absent(tmp_path):
    urls = Urls()
    sup = await supervised(tmp_path, urls, metrics=False)
    try:
        await until(lambda: urls.seen, timeout=10)
        assert urls.seen == [url(1)], cf_log(tmp_path)
    finally:
        await sup.close()


async def test_banner_of_an_earlier_run_in_the_log_is_ignored(tmp_path, caplog):
    caplog.set_level(logging.INFO, logger="claude_voice.cloudflared")
    (tmp_path / "logs").mkdir()
    (tmp_path / "logs" / "cloudflared.log").write_text("INF |  https://stale-old.trycloudflare.com  |\n")
    urls = Urls()
    sup = await supervised(tmp_path, urls, url_timeout=0.5, metrics=False, banner=False)
    try:
        await until(lambda: len(runs(tmp_path)) >= 2, timeout=10)
        assert [u for u in urls.seen if u] == [], cf_log(tmp_path)
    finally:
        await sup.close()


@pytest.mark.parametrize(
    "mode",
    [
        {"metrics_value": "evil.example.com", "banner": False},
        {"metrics_value": "abc.trycloudflare.com.evil.example", "banner_host": "abc.trycloudflare.com.evil.example"},
        {"metrics": False, "banner_host": "abc.trycloudflare.com/x"},
    ],
    ids=["other-domain", "suffix-attack", "banner-with-path"],
)
async def test_hostname_off_the_pattern_is_never_pushed(tmp_path, caplog, mode):
    caplog.set_level(logging.WARNING, logger="claude_voice.cloudflared")
    urls = Urls()
    sup = await supervised(tmp_path, urls, url_timeout=0.5, **mode)
    try:
        await until(lambda: len(runs(tmp_path)) >= 2, timeout=10)
        assert [u for u in urls.seen if u] == [], cf_log(tmp_path)
        text = "\n".join(r.getMessage() for r in caplog.records)
        assert "no tunnel hostname within 0.5 s" in text, text
        if "metrics_value" in mode:
            assert "is not a Quick Tunnel host; ignored" in text, text
    finally:
        await sup.close()


async def test_crash_restarts_with_a_new_hostname_pushed_to_the_active_session(tmp_path):
    h = make()
    await h.up()
    sup = await supervised(tmp_path, h.link.set_remote, cfg=h.cfg)
    try:
        await until(lambda: h.dev.remotes, timeout=10)
        assert h.dev.remotes == [{"url": url(1)}], cf_log(tmp_path)
        (tmp_path / "cf" / "crash").write_text("")
        await until(lambda: len(h.dev.remotes) == 2, timeout=10)
        assert h.dev.remotes == [{"url": url(1)}, {"url": url(2)}], cf_log(tmp_path)
        assert sup.url == url(2)
        assert not alive(runs(tmp_path)[0]["pid"])
    finally:
        await sup.close()
        await h.down()


def _delays(caplog):
    return [float(r.getMessage().split()[-2]) for r in caplog.records if r.getMessage().startswith("restarting")]


async def test_restart_delay_grows_and_resets_after_a_long_run(tmp_path, caplog):
    caplog.set_level(logging.INFO, logger="claude_voice.cloudflared")
    sup = await supervised(tmp_path, restart_max=1.5, lifetimes=[0, 0, 0, 2.0, 0, 0])
    try:
        await until(lambda: len(_delays(caplog)) >= 6, timeout=15)
        assert _delays(caplog)[:6] == [0.05, 0.1, 0.2, 0.05, 0.1, 0.2], cf_log(tmp_path)
    finally:
        await sup.close()


@pytest.mark.parametrize("ignore_term", [False, True], ids=["terminates", "needs-kill"])
async def test_close_stops_the_child_and_the_log(tmp_path, ignore_term):
    urls = Urls()
    sup = await supervised(tmp_path, urls, ignore_term=ignore_term)
    await until(lambda: urls.seen, timeout=10)
    pid = runs(tmp_path)[0]["pid"]
    assert alive(pid)
    async with asyncio.timeout(5):
        await sup.close()
    assert not alive(pid), cf_log(tmp_path)
    assert sup._log_file is None and sup.url is None


async def test_missing_binary_retries_without_raising(tmp_path, caplog):
    caplog.set_level(logging.WARNING, logger="claude_voice.cloudflared")
    from claude_voice.config import Config

    c = cf_cfg(Config(), tmp_path, tmp_path / "no-such-cloudflared")
    sup = Cloudflared(c, 47999, Urls())
    sup.start()
    try:
        await until(lambda: sum("cloudflared failed" in r.getMessage() for r in caplog.records) >= 2, timeout=5)
        assert not sup._task.done()
    finally:
        await sup.close()


async def test_readiness_timeout_leaves_the_relay_running(tmp_path):
    h = make()
    await h.up()
    sup = await supervised(tmp_path, h.link.set_remote, cfg=h.cfg, url_timeout=0.3, metrics=False, banner=False)
    try:
        await until(lambda: len(runs(tmp_path)) >= 2, timeout=10)
        h.dev.inject(TURN_INPUT)
        await settle(h.dev, State.LISTENING)
        assert h.dev.remotes == [] and not sup._task.done(), cf_log(tmp_path)
    finally:
        await sup.close()
        await h.down()


async def test_cloudflared_starts_only_with_a_binary_and_a_listening_tunnel(tmp_path):
    from test_tunnel import tunnel, tunnel_cfg, Recorder

    binary = fake_cloudflared(tmp_path)
    c = cf_cfg(tunnel_cfg(), tmp_path, binary)
    link = Link(c, lambda: None, None)
    assert cli.start_cloudflared(dataclasses.replace(c, cloudflared_bin=""), link, None) is None
    assert cli.start_cloudflared(c, link, None) is None
    async with tunnel(Recorder(), c) as srv:
        assert cli.start_cloudflared(dataclasses.replace(c, cloudflared_bin=""), link, srv) is None
        await asyncio.sleep(0.1)
        assert runs(tmp_path) == []
        sup = cli.start_cloudflared(c, link, srv)
        try:
            await until(lambda: sup.url, timeout=10)
            assert runs(tmp_path)[0]["argv"][3] == f"http://127.0.0.1:{srv.port}", cf_log(tmp_path)
        finally:
            await sup.close()


async def test_set_remote_after_hello_on_usb_and_again_on_change():
    h = make()
    h.link.set_remote(url(1))
    await h.up()
    try:
        assert h.dev.hellos == ["boot", "reply"] and h.dev.remotes == [{"url": url(1)}] * 2
        h.link.set_remote(url(1))
        h.link.set_remote(url(2))
        await until(lambda: len(h.dev.remotes) == 3)
        h.link.set_remote(None)
        await asyncio.sleep(0.1)
        assert h.dev.remotes == [{"url": url(1)}] * 2 + [{"url": url(2)}]
    finally:
        await h.down()


async def test_no_set_remote_while_no_url_is_known():
    h = make()
    await h.up()
    try:
        h.dev.inject(TURN_INPUT)
        await settle(h.dev, State.LISTENING)
        assert T.SET_REMOTE not in h.dev.received
    finally:
        await h.down()


async def test_set_remote_after_hello_on_lan_and_never_on_an_inactive_session():
    async with WifiHarness() as w:
        w.h.link.set_remote(url(1))
        d = await w.board()
        await until(lambda: d.remotes)
        assert d.remotes == [{"url": url(1)}]
        w.h.allow_open = True
        await until(lambda: w.h.devices and len(w.h.dev.remotes) == 2)
        assert w.h.link.active_kind == "usb" and w.h.dev.remotes == [{"url": url(1)}] * 2
        t = await tls_connect(w.srv.port, IDENTITY, KEY)
        late = FakeDevice(t, link="wifi")
        late.start()
        w.boards.append(late)
        late.hello("boot")
        w.h.link.set_remote(url(2))
        await until(lambda: len(w.h.dev.remotes) == 3)
        with pytest.raises(TransportClosed):
            for _ in range(40):
                await t.read(0.05)
        assert late.remotes == [] and d.remotes == [{"url": url(1)}]


async def test_set_remote_after_hello_on_the_tunnel():
    async with TunnelHarness() as w:
        w.h.link.set_remote(url(1))
        d = await w.board()
        await until(lambda: d.remotes)
        w.h.link.set_remote(url(2))
        await until(lambda: len(d.remotes) == 2)
        assert d.remotes == [{"url": url(1)}, {"url": url(2)}]


@pytest.mark.parametrize(
    "line",
    ["cloudflared_url_timeout_s = 0.0", "cloudflared_restart_min_s = 0.0", "cloudflared_restart_max_s = -1.0"],
)
def test_config_rejects_bad_cloudflared_values(tmp_path, line):
    p = tmp_path / "c.toml"
    p.write_text(line + "\n", encoding="utf-8")
    with pytest.raises(ValueError, match=line.split()[0]):
        load(p)


def test_config_rejects_restart_max_below_min(tmp_path):
    p = tmp_path / "c.toml"
    p.write_text("cloudflared_restart_min_s = 5.0\ncloudflared_restart_max_s = 2.0\n", encoding="utf-8")
    with pytest.raises(ValueError, match="cloudflared_restart_max_s"):
        load(p)
