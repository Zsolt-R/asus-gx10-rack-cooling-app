"""GX-RACK: dashboard, REST API and MCP server for the DGX rack fan controller.

One process:
  * polls the controller's /api/status every POLL_SECONDS and keeps history
    in SQLite (one row every SAMPLE_SECONDS, RETAIN_DAYS kept);
  * serves the dashboard at /, JSON at /api/*;
  * serves the MCP server "GX-RACK" (streamable HTTP) at /mcp.

Reading is open. Control needs either the controller's own web user/password
(HTTP Basic -- what the browser asks for) or the API key (X-API-Key header).
/mcp always needs the API key. The controller password never leaves this
process except towards the controller itself.
"""
import asyncio
import base64
import contextlib
import hmac
import logging
import os
import sqlite3
import time
from pathlib import Path
from typing import Literal

import httpx
from mcp.server.fastmcp import FastMCP
from mcp.server.transport_security import TransportSecuritySettings
from starlette.applications import Starlette
from starlette.requests import Request
from starlette.responses import FileResponse, JSONResponse, Response
from starlette.routing import Mount, Route

CONTROLLER = os.environ.get("CONTROLLER_URL", "http://dgx-fans.local").rstrip("/")
CTRL_USER = os.environ["CONTROLLER_USER"]
CTRL_PASS = os.environ["CONTROLLER_PASS"]
API_KEY = os.environ["MCP_API_KEY"]
DB_PATH = os.environ.get("DB_PATH", "/data/history.db")
POLL_SECONDS = float(os.environ.get("POLL_SECONDS", "2"))
SAMPLE_SECONDS = 10
RETAIN_DAYS = 7
STATIC = Path(__file__).parent / "static"

# httpx logs every poll at INFO; keep the log for things that matter.
logging.getLogger("httpx").setLevel(logging.WARNING)

# Mirrors controller/src/main.cpp -- keep in step when the firmware changes.
SYSTEM = {
    "what": "Two Noctua 120 mm PWM fans blow up into the bottom intakes of two ASUS "
            "Ascent GX10s. Fan N follows probe N, taped at the back of fan N's shroud "
            "in GX10-heated air. Idle GX10: probe < 30 C; heavy load: 45-55 C.",
    "curve": [{"probe_c": "<30", "duty_pct": 0}, {"probe_c": 30, "duty_pct": 20},
              {"probe_c": 35, "duty_pct": 30}, {"probe_c": 40, "duty_pct": 45},
              {"probe_c": 45, "duty_pct": 65}, {"probe_c": 50, "duty_pct": 85},
              {"probe_c": ">=55", "duty_pct": 100}],
    "profiles": {"normal": "curve, capped at 80 % (~2400 RPM)",
                 "quiet": "curve x 67 % (~2000 RPM at the top)",
                 "max": "always 100 % (~3000 RPM)"},
    "priority_per_fan": [
        "1. turned off (set_fan_power on=false) -> 0 %, nothing overrides it, not even heat",
        "2. identify -> that fan 100 %, the other 0 %, 15 s",
        "3. USB console set/max hold -> fixed duty, 10 min",
        "4. max profile with the physical switch on -> 100 %",
        "5. probe fault -> 0 % (no full-speed failsafe, by design)",
        "6. curve: switch off -> 0 % unless probe >= 55 C; quiet x 67 %; normal cap 80 %",
    ],
    "notes": [
        "A controller restart clears 'turned off' and returns to Normal.",
        "Flipping the physical switch turns both fans back on.",
        "Expected RPM: ~660 at 20 %, ~1400 at 45 %, ~2400 at 80 %, ~3000 at 100 %.",
        "The real limit is inside the GX10 (board sensor 'acpitz', shuts off ~94-96 C).",
    ],
}

state = {"status": None, "at": 0.0, "error": "not polled yet"}


# ------------------------------------------------------------------ history --
def db():
    con = sqlite3.connect(DB_PATH)
    con.execute("""CREATE TABLE IF NOT EXISTS samples (
        ts INTEGER PRIMARY KEY, t0 REAL, t1 REAL, duty0 INTEGER, duty1 INTEGER,
        rpm0 INTEGER, rpm1 INTEGER, profile TEXT, switch_on INTEGER)""")
    return con


def store(s):
    with contextlib.closing(db()) as con, con:
        con.execute("INSERT OR REPLACE INTO samples VALUES (?,?,?,?,?,?,?,?,?)",
                    (int(time.time()), s["temp"][0], s["temp"][1], s["duty"][0],
                     s["duty"][1], s["rpm"][0], s["rpm"][1], s["profile"],
                     int(s["switchOn"])))
        con.execute("DELETE FROM samples WHERE ts < ?",
                    (int(time.time()) - RETAIN_DAYS * 86400,))


def history(minutes, max_points=300):
    minutes = max(1, min(int(minutes), RETAIN_DAYS * 1440))
    since = int(time.time()) - minutes * 60
    bucket = max(SAMPLE_SECONDS, minutes * 60 // max(1, int(max_points)))
    with contextlib.closing(db()) as con:
        rows = con.execute(
            """SELECT (ts / ?) * ? AS b, AVG(t0), AVG(t1), AVG(duty0), AVG(duty1),
                      AVG(rpm0), AVG(rpm1)
               FROM samples WHERE ts >= ? GROUP BY b ORDER BY b""",
            (bucket, bucket, since)).fetchall()
    r1 = lambda v: None if v is None else round(v, 1)
    r0 = lambda v: None if v is None else round(v)
    return {"minutes": minutes, "bucket_seconds": bucket,
            "points": [{"ts": b, "temp": [r1(a), r1(c)], "duty": [r0(d), r0(e)],
                        "rpm": [r0(f), r0(g)]} for b, a, c, d, e, f, g in rows]}


# --------------------------------------------------------------- controller --
async def poller():
    last_sample = 0.0
    async with httpx.AsyncClient(timeout=5) as client:
        while True:
            try:
                r = await client.get(f"{CONTROLLER}/api/status")
                r.raise_for_status()
                state.update(status=r.json(), at=time.time(), error=None)
                if time.time() - last_sample >= SAMPLE_SECONDS:
                    last_sample = time.time()
                    await asyncio.to_thread(store, state["status"])
            except Exception as e:  # keep polling whatever happens
                state["error"] = f"{type(e).__name__}: {e}"
            await asyncio.sleep(POLL_SECONDS)


async def refresh():
    async with httpx.AsyncClient(timeout=5) as client:
        r = await client.get(f"{CONTROLLER}/api/status")
        r.raise_for_status()
        state.update(status=r.json(), at=time.time(), error=None)


async def command(path, data):
    """POST to the controller with its own credentials; returns (ok, message)."""
    async with httpx.AsyncClient(timeout=5, auth=(CTRL_USER, CTRL_PASS)) as client:
        r = await client.post(f"{CONTROLLER}{path}", data=data)
    if r.status_code == 200:
        await asyncio.sleep(0.3)          # let the next control tick apply it
        with contextlib.suppress(Exception):
            await refresh()
        return True, "ok"
    return False, f"controller answered {r.status_code}: {r.text[:200]}"


def summary():
    s, age = state["status"], time.time() - state["at"]
    if s is None:
        return {"controller_reachable": False, "error": state["error"]}
    fans = []
    for i in (0, 1):
        fans.append({"fan": i + 1, "probe_temp_c": s["temp"][i],
                     "probe_fault": s["temp"][i] is None, "duty_pct": s["duty"][i],
                     "rpm": s["rpm"][i], "turned_off": s.get("off", [False, False])[i],
                     "overheat_55c": s["overheat"][i]})
    return {"controller_reachable": state["error"] is None and age < 15,
            "age_seconds": round(age, 1), "error": state["error"],
            "profile": s["profile"], "switch_on": s["switchOn"], "fans": fans,
            "identify": {"fan": s.get("identify") or None,
                         "seconds_left": s.get("identifyLeft", 0)},
            "console_hold": s["manual"], "wifi_rssi_dbm": s["rssi"],
            "controller_uptime_s": s["uptime"], "raw": s}


async def set_profile(p):
    return await command("/api/profile", {"name": p})


async def identify(fan):              # fan 0 = stop
    return await command("/api/identify", {"fan": str(fan)})


async def fan_power(fan, on):
    return await command("/api/fan", {"fan": str(fan), "on": "1" if on else "0"})


# ---------------------------------------------------------------------- MCP --
mcp = FastMCP(
    "GX-RACK",
    instructions=(
        "Controls the two fans cooling the ASUS GX10s in the DGX rack. Read with "
        "get_status / get_history; describe_system explains the curve, profiles and "
        "which control wins. The fans cool real machines under load: before stopping "
        "a fan or changing the profile, say what you are doing, and put back what the "
        "user had afterwards unless asked to leave it."),
    stateless_http=True,
    json_response=True,
    streamable_http_path="/mcp",
    # Reached by IP and through reverse proxies; the API key is the guard.
    transport_security=TransportSecuritySettings(enable_dns_rebinding_protection=False),
)


def result(ok_msg):
    ok, msg = ok_msg
    return {"ok": ok, "message": msg, "status": summary()}


@mcp.tool()
async def get_status() -> dict:
    """Current readings: each fan's probe temperature (C), duty (%), RPM, whether it
    is turned off or over 55 C, plus the profile, physical switch, identify state,
    Wi-Fi signal and controller uptime. probe_temp_c is null when the probe is faulty."""
    with contextlib.suppress(Exception):
        await refresh()
    return summary()


@mcp.tool()
async def get_history(minutes: int = 60, max_points: int = 120) -> dict:
    """Temperature, duty and RPM history, averaged into at most max_points buckets.
    minutes: how far back (1 .. 10080 = 7 days). Index 0 = fan/probe 1, 1 = fan/probe 2."""
    return await asyncio.to_thread(history, minutes, max(10, min(max_points, 1000)))


@mcp.tool(name="set_profile")
async def set_profile_tool(profile: Literal["normal", "quiet", "max"]) -> dict:
    """Set the profile for both fans: normal (curve, max 80 %), quiet (curve x 67 %)
    or max (100 %). Also ends a running identify."""
    return result(await set_profile(profile))


@mcp.tool()
async def identify_fan(fan: Literal[1, 2]) -> dict:
    """Run this fan at 100 % and stop the other for 15 s, so a person can see which
    is which. Use stop_identify to end it early."""
    return result(await identify(fan))


@mcp.tool()
async def stop_identify() -> dict:
    """End a running identify; both fans return to normal control."""
    return result(await identify(0))


@mcp.tool()
async def set_fan_power(fan: Literal[1, 2], on: bool) -> dict:
    """Turn one fan off (on=false) or back on (on=true). Off is a HARD stop: the fan
    stays stopped even if its GX10 overheats, until turned on again, the physical
    switch is flipped or the controller restarts. Confirm with the user before
    turning a fan off while its probe is above ~45 C."""
    return result(await fan_power(fan, on))


@mcp.tool()
async def describe_system() -> dict:
    """How the system works: hardware, temperature curve, profiles, the order in which
    controls override each other, expected RPM."""
    return SYSTEM


# --------------------------------------------------------------------- HTTP --
def authorised(request: Request) -> bool:
    key = request.headers.get("x-api-key", "")
    if key and hmac.compare_digest(key, API_KEY):
        return True
    auth = request.headers.get("authorization", "")
    if auth.lower().startswith("basic "):
        with contextlib.suppress(Exception):
            user, _, pw = base64.b64decode(auth[6:]).decode().partition(":")
            return (hmac.compare_digest(user, CTRL_USER)
                    and hmac.compare_digest(pw, CTRL_PASS))
    return False


def denied():
    return Response("login required", status_code=401,
                    headers={"WWW-Authenticate": 'Basic realm="GX-RACK"'})


async def page(request):
    return FileResponse(STATIC / "index.html", headers={"Cache-Control": "no-store"})


async def api_status(request):
    return JSONResponse(summary(), headers={"Cache-Control": "no-store"})


async def api_history(request):
    q = request.query_params
    return JSONResponse(await asyncio.to_thread(
        history, q.get("minutes", "60"), q.get("points", "300")))


async def api_system(request):
    return JSONResponse(SYSTEM)


async def form(request):
    if request.headers.get("content-type", "").startswith("application/json"):
        return await request.json()
    return dict(await request.form())


async def api_profile(request):
    if not authorised(request):
        return denied()
    p = (await form(request)).get("name", "")
    if p not in ("normal", "quiet", "max"):
        return JSONResponse({"ok": False, "message": "name must be normal|quiet|max"}, 400)
    return JSONResponse(result(await set_profile(p)))


async def api_identify(request):
    if not authorised(request):
        return denied()
    fan = str((await form(request)).get("fan", ""))
    if fan not in ("0", "1", "2"):
        return JSONResponse({"ok": False, "message": "fan must be 0 (stop), 1 or 2"}, 400)
    return JSONResponse(result(await identify(int(fan))))


async def api_fan(request):
    if not authorised(request):
        return denied()
    f = await form(request)
    fan, on = str(f.get("fan", "")), str(f.get("on", "")).lower()
    if fan not in ("1", "2") or on not in ("0", "1", "true", "false"):
        return JSONResponse({"ok": False, "message": "need fan=1|2 and on=0|1"}, 400)
    return JSONResponse(result(await fan_power(int(fan), on in ("1", "true"))))


async def api_login(request):
    """The dashboard calls this to make the browser ask for the login once."""
    return JSONResponse({"ok": True}) if authorised(request) else denied()


async def health(request):
    return JSONResponse({"ok": True, "controller": state["error"] is None})


class ApiKeyGate:
    """/mcp needs the API key (header X-API-Key or Authorization: Bearer)."""

    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send):
        if scope["type"] == "http" and scope["path"].startswith("/mcp"):
            h = {k.decode().lower(): v.decode() for k, v in scope["headers"]}
            key = h.get("x-api-key") or h.get("authorization", "").removeprefix("Bearer ")
            if not (key and hmac.compare_digest(key, API_KEY)):
                await JSONResponse({"error": "missing or wrong API key"}, 401)(
                    scope, receive, send)
                return
        await self.app(scope, receive, send)


@contextlib.asynccontextmanager
async def lifespan(app):
    Path(DB_PATH).parent.mkdir(parents=True, exist_ok=True)
    db().close()
    async with mcp.session_manager.run():
        task = asyncio.create_task(poller())
        try:
            yield
        finally:
            task.cancel()


mcp_app = mcp.streamable_http_app()

app = ApiKeyGate(Starlette(
    routes=[
        Route("/", page),
        Route("/health", health),
        Route("/api/status", api_status),
        Route("/api/history", api_history),
        Route("/api/system", api_system),
        Route("/api/login", api_login),
        Route("/api/profile", api_profile, methods=["POST"]),
        Route("/api/identify", api_identify, methods=["POST"]),
        Route("/api/fan", api_fan, methods=["POST"]),
        Mount("/", app=mcp_app),
    ],
    lifespan=lifespan,
))
