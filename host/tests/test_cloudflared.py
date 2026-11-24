import asyncio
import dataclasses
import json
import logging
import os
import shutil
import signal
import stat
import sys
import time

import pytest
from fake_device import FakeDevice, tls_connect
from test_relay import make, settle, until
from test_tunnel import TURN_INPUT
from test_tunnel import Harness as TunnelHarness
from test_wifi import IDENTITY, KEY, WifiHarness

from claude_voice import cli
from claude_voice import cloudflared as cloudflared_mod
from claude_voice.cloudflared import Backoff, Cloudflared, hostname_from_banner, remote_url
from claude_voice.config import load
from claude_voice.link import Link, TransportClosed, memory_pair
from claude_voice.protocol import PROTO_VERSION, SYNC, Decoder, State, T, encode

FAKE_CLOUDFLARED = r'''
import json, os, signal, stat, sys, time
here = os.path.dirname(os.path.abspath(__file__))
mode = json.load(open(os.path.join(here, "mode.json")))
runs_path = os.path.join(here, "runs")
n = int(open(runs_path).read()) + 1 if os.path.exists(runs_path) else 1
open(runs_path, "w").write(str(n))
args = sys.argv[1:]
config = args[args.index("--config") + 1] if "--config" in args else None
record = {
    "argv": args, "pid": os.getpid(), "tunnel_env": sorted(k for k in os.environ if k.startswith("TUNNEL_")),
    "keep_env": os.environ.get("CLAWD_TEST_KEEP"), "config": open(config).read() if config else None,
    "config_dir_mode": stat.S_IMODE(os.stat(os.path.dirname(config)).st_mode) if config else None,
}
with open(os.path.join(here, "argv.jsonl"), "a") as f:
    f.write(json.dumps(record) + "\n")
if mode.get("ignore_term"):
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
lifetimes = mode.get("lifetimes", [])
life = lifetimes[n - 1] if n <= len(lifetimes) else None
if life == 0:
    sys.exit(1)
host = mode.get("hostname", "run{n}-quick.trycloudflare.com").format(n=n)
def info(text):
    print(time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()) + " INF " + text, file=sys.stderr, flush=True)
info("Requesting new quick Tunnel on trycloudflare.com...")
if mode.get("banner", True):
    row = mode.get("banner_row", "https://" + mode.get("banner_host", host))
    info("+" + "-" * 92 + "+")
    info("|  Your quick Tunnel has been created! Visit it at (it may take some time to be reachable):  |")
    info("|  " + row.ljust(88) + "  |")
    info("+" + "-" * 92 + "+")
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
    sup = Cloudflared(c, port, on_url or Urls(), grace_s=0.3)
    sup.start()
    return sup


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def test_hostname_patterns():
    good = "2026-11-20T10:00:00Z INF |  https://abc-1.trycloudflare.com                            |"
    assert hostname_from_banner(good) == "abc-1.trycloudflare.com"
    assert hostname_from_banner("INF |https://abc-1.trycloudflare.com |") == "abc-1.trycloudflare.com"
    assert hostname_from_banner("https://abc-1.trycloudflare.com\n") == "abc-1.trycloudflare.com"
    for bad in (
        "https://abc.trycloudflare.com.evil.example |", "https://abc.trycloudflare.com/path |",
        "https://abc.trycloudflare.com:8443 |", "http://abc.trycloudflare.com |", "https://ABC.trycloudflare.com |",
        "https://a_b.trycloudflare.com |", "https://evil.example |", "INF |  xhttps://a-b.trycloudflare.com  |",
        "INF |  =https://a-b.trycloudflare.com  |", "INF evil.example/https://a-b.trycloudflare.com  |",
    ):
        assert hostname_from_banner(bad) is None, bad
    assert remote_url("abc.trycloudflare.com") == "wss://abc.trycloudflare.com/clawd/v1"


def test_backoff_doubles_to_the_maximum_and_resets_after_a_long_run():
    b = Backoff(1.0, 8.0)
    assert [b.after(0.5) for _ in range(5)] == [1.0, 2.0, 4.0, 8.0, 8.0]
    assert b.after(8.0) == 8.0
    assert b.after(8.1) == 1.0 and b.after(0.1) == 2.0


async def test_hostname_from_the_banner_with_the_exact_arguments_and_no_http(tmp_path, monkeypatch):
    import httpx

    requests = []
    monkeypatch.setattr(httpx.AsyncClient, "send", lambda self, req, **kw: requests.append(req))
    urls = Urls()
    sup = await supervised(tmp_path, urls)
    try:
        await until(lambda: urls.seen, timeout=10)
        assert urls.seen == [url(1)] and sup.url == url(1), cf_log(tmp_path)
        run = runs(tmp_path)[0]
        config = run["argv"][3]
        assert run["argv"] == ["tunnel", "--no-autoupdate", "--config", config, "--url", "http://127.0.0.1:47999"]
        assert run["config"] == "{}\n" and run["config_dir_mode"] == 0o700
        assert "--metrics" not in run["argv"] and requests == [] and not hasattr(cloudflared_mod, "httpx")
    finally:
        await sup.close()
    assert not os.path.exists(os.path.dirname(config))


async def test_child_gets_no_tunnel_variables_and_a_fresh_config_on_every_start(tmp_path, monkeypatch):
    for name in ("TUNNEL_URL", "TUNNEL_TOKEN", "TUNNEL_METRICS", "TUNNEL_ORIGIN_CERT"):
        monkeypatch.setenv(name, "http://127.0.0.1:1")
    monkeypatch.setenv("CLAWD_TEST_KEEP", "kept")
    urls = Urls()
    sup = await supervised(tmp_path, urls)
    try:
        await until(lambda: urls.seen, timeout=10)
        config = runs(tmp_path)[0]["argv"][3]
        with open(config, "w") as f:
            f.write("ingress:\n  - service: http://127.0.0.1:1\n")
        (tmp_path / "cf" / "crash").write_text("")
        await until(lambda: url(2) in urls.seen, timeout=10)
        first, second = runs(tmp_path)[:2]
        assert first["tunnel_env"] == second["tunnel_env"] == [], cf_log(tmp_path)
        assert first["keep_env"] == second["keep_env"] == "kept"
        assert second["argv"][3] == config and second["config"] == "{}\n"
        shutil.rmtree(os.path.dirname(config))
        (tmp_path / "cf" / "crash").write_text("")
        await until(lambda: url(3) in urls.seen, timeout=10)
        third = runs(tmp_path)[2]
        assert third["config"] == "{}\n" and third["config_dir_mode"] == 0o700, cf_log(tmp_path)
    finally:
        await sup.close()
    assert not os.path.exists(os.path.dirname(config))
    assert not os.path.exists(os.path.dirname(third["argv"][3]))


async def test_banner_of_an_earlier_run_in_the_log_is_ignored(tmp_path, caplog):
    caplog.set_level(logging.INFO, logger="claude_voice.cloudflared")
    (tmp_path / "logs").mkdir()
    (tmp_path / "logs" / "cloudflared.log").write_text("INF |  https://stale-old.trycloudflare.com  |\n")
    urls = Urls()
    sup = await supervised(tmp_path, urls, url_timeout=0.5, banner=False)
    try:
        await until(lambda: len(runs(tmp_path)) >= 2, timeout=10)
        assert [u for u in urls.seen if u] == [], cf_log(tmp_path)
    finally:
        await sup.close()


@pytest.mark.parametrize(
    "mode",
    [
        {"banner_host": "evil.example.com"},
        {"banner_host": "abc.trycloudflare.com.evil.example"},
        {"banner_host": "abc.trycloudflare.com/x"},
        {"banner_row": "xhttps://a-b.trycloudflare.com"},
    ],
    ids=["other-domain", "suffix-attack", "banner-with-path", "no-left-boundary"],
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
    sup = await supervised(tmp_path, h.link.set_remote, cfg=h.cfg, url_timeout=0.3, banner=False)
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
            argv = runs(tmp_path)[0]["argv"]
            assert argv[argv.index("--url") + 1] == f"http://127.0.0.1:{srv.port}", cf_log(tmp_path)
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


class DropsRemote:
    """Host transport that drops the next `drop` SET_REMOTE frames the way a full send buffer does."""

    def __init__(self, t):
        self.t, self.drop, self.attempts = t, 0, []

    async def read(self, timeout):
        return await self.t.read(timeout)

    def write(self, data):
        if data[len(SYNC)] == T.SET_REMOTE:
            ok = self.drop == 0
            self.drop = max(self.drop - 1, 0)
            self.attempts.append(ok)
            if not ok:
                return False
        return self.t.write(data)

    async def close(self):
        await self.t.close()


async def test_dropped_set_remote_is_resent_within_a_ping_interval_and_after_each_hello():
    h = make()
    opener, wrapped = h.link._opener, []

    def open_dropping():
        t = opener()
        if t is not None:
            wrapped.append(t := DropsRemote(t))
        return t

    h.link._opener = open_dropping
    await h.up()
    try:
        await until(lambda: h.dev.hellos == ["boot", "reply"])
        await asyncio.sleep(0.1)
        first = wrapped[0]
        first.drop = 1
        h.link.set_remote(url(1))
        await until(lambda: h.dev.remotes, timeout=2.0)
        await asyncio.sleep(h.cfg.ping_interval_s * 6)
        assert first.attempts == [False, True] and h.dev.remotes == [{"url": url(1)}]
        assert h.dev.hellos == ["boot", "reply"]
        h.dev.hello("resume")
        await until(lambda: len(h.dev.remotes) == 2)
        await asyncio.sleep(h.cfg.ping_interval_s * 4)
        assert first.attempts == [False, True, True]
        await first.close()
        await until(lambda: len(wrapped) == 2 and h.dev.remotes)
        assert h.dev.hellos and wrapped[1].attempts and all(wrapped[1].attempts)
    finally:
        await h.down()


async def test_no_set_remote_on_ping_ticks_before_hello():
    h = make()
    h.allow_open = False
    h.link.set_remote(url(1))
    host, board = memory_pair()
    task = asyncio.create_task(h.link.serve_wifi(host))
    dec, got = Decoder(None), []

    async def drain(done, timeout=5.0):
        end = time.monotonic() + timeout
        while not done() and time.monotonic() < end:
            got.extend(f for f, _ in dec.feed(await board.read(0.02), time.monotonic()))

    try:
        await drain(lambda: got.count(T.PING) >= 3)
        assert got.count(T.PING) >= 3 and T.SET_REMOTE not in got, got
        hello = {"proto": PROTO_VERSION, "fw": "fake", "rate": 16000, "reason": "boot", "link": "wifi"}
        board.write(encode(T.HELLO, json.dumps(hello).encode()))
        await drain(lambda: T.SET_REMOTE in got)
        pings = got.count(T.PING)
        await drain(lambda: got.count(T.PING) >= pings + 3)
        assert got.count(T.SET_REMOTE) == 1 and got.count(T.PING) >= pings + 3, got
    finally:
        await host.close()
        await task


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


async def test_relay_waits_for_the_guard_to_kill_a_child_that_ignores_sigterm(tmp_path):
    urls = Urls()
    sup = await supervised(tmp_path, urls, ignore_term=True)
    await until(lambda: urls.seen, timeout=10)
    guard, pid = sup._proc, runs(tmp_path)[0]["pid"]
    await sup.close()
    assert not alive(pid) and guard.returncode == 128 + signal.SIGKILL, cf_log(tmp_path)


RELAY_STUB = r'''
import asyncio, dataclasses, sys
from claude_voice.cloudflared import Cloudflared
from claude_voice.config import Config

async def main(binary, log_dir):
    cfg = dataclasses.replace(Config(), cloudflared_bin=binary, log_dir=log_dir, cloudflared_url_timeout_s=10.0)
    Cloudflared(cfg, 47999, lambda url: None).start()
    print("up", flush=True)
    await asyncio.sleep(3600)

asyncio.run(main(sys.argv[1], sys.argv[2]))
'''


async def test_cloudflared_stops_when_the_relay_is_killed(tmp_path):
    binary = fake_cloudflared(tmp_path)
    (tmp_path / "relay_stub.py").write_text(RELAY_STUB)
    relay = await asyncio.create_subprocess_exec(
        sys.executable, str(tmp_path / "relay_stub.py"), str(binary), str(tmp_path / "logs"),
        stdout=asyncio.subprocess.PIPE,
    )
    pid = None
    try:
        await asyncio.wait_for(relay.stdout.readline(), 10)
        await until(lambda: runs(tmp_path), timeout=10)
        pid = runs(tmp_path)[0]["pid"]
        assert alive(pid)
        relay.kill()
        await relay.wait()
        await until(lambda: not alive(pid), timeout=10)
    finally:
        if pid is not None and alive(pid):
            os.kill(pid, signal.SIGKILL)
        if relay.returncode is None:
            relay.kill()
            await relay.wait()
