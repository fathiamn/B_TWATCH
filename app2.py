"""
T-Watch Hiking Band — Pi Receiver
----------------------------------
Install:
    pip install flask flask-cors bleak realtime zeroconf --break-system-packages
Run:
    python app.py
"""

import asyncio
import json
import os
import socket
import subprocess
import threading
from datetime import datetime

from flask import Flask, jsonify, request
from flask_cors import CORS
from realtime import AsyncRealtimeClient
from zeroconf import ServiceInfo, Zeroconf

# ── Config ────────────────────────────────────────────────────────────────────

SUPABASE_URL = os.environ.get(
    "SUPABASE_URL",
    "https://sudlejmejjlairgxdlzi.supabase.co",
)
SUPABASE_ANON_KEY = os.environ.get(
    "SUPABASE_ANON_KEY",
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InN1ZGxlam1lampsYWlyZ3hkbHppIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzM1Nzc5OTIsImV4cCI6MjA4OTE1Mzk5Mn0.NRQy1vGT3LnbO1oo_yDoeVjOxz4xL9ErscJWNT1bAQo",
)
CHANNEL_NAME      = "twatch-activity"
WATCH_NAME        = "Espruino (T-Watch2020V2)"
WATCH_ADDRESS     = "08:3A:F2:69:AA:96"   # hardcoded — avoids scan entirely
UART_TX_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
MDNS_SERVICE_NAME = "hiking-pi"
SERVER_PORT       = 5000
SESSION_JSON      = "last_session.json"
SESSION_TXT       = "last_session.txt"
HISTORY_JSON      = "session_history.json"
HISTORY_MAX       = 10

# ── Live state cache ──────────────────────────────────────────────────────────

_live_state = {
    "session_active": False,
    "steps": 0, "distance": 0, "duration": 0,
    "calories": 0, "source": "none",
}
_live_lock = threading.Lock()


def update_live_cache(steps, distance, duration, source):
    with _live_lock:
        _live_state.update({
            "session_active": True,
            "steps": steps, "distance": distance,
            "duration": duration, "calories": _calories(steps),
            "source": source,
        })


def clear_live_cache():
    with _live_lock:
        _live_state.update({
            "session_active": False,
            "steps": 0, "distance": 0, "duration": 0,
            "calories": 0, "source": "none",
        })


def get_live_cache():
    with _live_lock:
        return dict(_live_state)

# ── Flask ─────────────────────────────────────────────────────────────────────

app = Flask(__name__)
CORS(app, origins="*")

# ── Helpers ───────────────────────────────────────────────────────────────────

def _calories(steps):
    return steps * 4 // 100


def get_local_ip():
    try:
        out = subprocess.check_output(["ip", "addr", "show", "wlan0"], text=True)
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("inet ") and "scope global" in line:
                return line.split()[1].split("/")[0]
    except Exception:
        pass
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    finally:
        s.close()

# ── mDNS ──────────────────────────────────────────────────────────────────────

def start_mdns():
    ip = get_local_ip()
    info = ServiceInfo(
        "_http._tcp.local.",
        f"{MDNS_SERVICE_NAME}._http._tcp.local.",
        addresses=[socket.inet_aton(ip)],
        port=SERVER_PORT,
        properties={"path": "/live"},
        server=f"{MDNS_SERVICE_NAME}.local.",
    )
    zc = Zeroconf()
    zc.register_service(info)
    print(f"[mDNS] {MDNS_SERVICE_NAME}.local at {ip}:{SERVER_PORT}")
    return zc

# ── File helpers ──────────────────────────────────────────────────────────────

def _calories(steps):
    return steps * 4 // 100


def save_last_session(steps, distance, duration):
    calories = _calories(steps)
    ended_at = datetime.now().isoformat(timespec="seconds")
    data = {
        "steps": steps, "distance": distance,
        "duration": duration, "calories": calories, "ended_at": ended_at,
    }
    try:
        with open(SESSION_JSON, "w") as f:
            json.dump(data, f, indent=2)
        print(f"[File] Saved session: {data}")
    except Exception as e:
        print(f"[File] Save error: {e}")
    return data


def load_last_session():
    try:
        with open(SESSION_JSON) as f:
            return json.load(f)
    except FileNotFoundError:
        return None
    except Exception as e:
        print(f"[File] Load error: {e}")
        return None


def load_history():
    try:
        with open(HISTORY_JSON) as f:
            data = json.load(f)
            if isinstance(data, list):
                return data
    except FileNotFoundError:
        pass
    except Exception as e:
        print(f"[File] History load error: {e}")
    return []


def save_history(history):
    try:
        with open(HISTORY_JSON, "w") as f:
            json.dump(history, f, indent=2)
    except Exception as e:
        print(f"[File] History save error: {e}")


def append_session_to_history(session):
    history = load_history()
    history.insert(0, {
        "steps":    session.get("steps",    0),
        "distance": session.get("distance", 0),
        "duration": session.get("duration", 0),
        "calories": session.get("calories", 0),
        "ended_at": session.get("ended_at", datetime.now().isoformat(timespec="seconds")),
    })
    history = history[:HISTORY_MAX]
    save_history(history)
    return history


def handle_session_end(steps, distance, duration, source):
    print(f"[{source.upper()}] session-end stp={steps} dst={distance}m dur={duration}s")
    clear_live_cache()
    session = save_last_session(steps, distance, duration)
    history = append_session_to_history(session)
    broadcast_now("session_end", {
        "source": source,
        "steps":    session["steps"],
        "distance": session["distance"],
        "duration": session["duration"],
        "calories": session["calories"],
        "ended_at": session["ended_at"],
        "history":  history,
    })
    return session

# ── Supabase broadcast via REST ───────────────────────────────────────────────

def _broadcast_via_rest(event, payload):
    url  = SUPABASE_URL.rstrip("/") + "/realtime/v1/api/broadcast"
    body = json.dumps({
        "messages": [{
            "topic":   CHANNEL_NAME,
            "event":   event,
            "payload": payload,
        }]
    })
    print(f"[Supabase] → {event}", flush=True)
    try:
        result = subprocess.run(
            [
                "curl", "-s", "-o", "/dev/null", "-w", "%{http_code}",
                "-X", "POST", url,
                "-H", "Content-Type: application/json",
                "-H", f"Authorization: Bearer {SUPABASE_ANON_KEY}",
                "-H", f"apikey: {SUPABASE_ANON_KEY}",
                "-d", body,
            ],
            capture_output=True, text=True, timeout=8,
        )
        code = result.stdout.strip()
        if result.returncode == 0 and code.startswith("2"):
            print(f"[Supabase] OK: {event} ({code})", flush=True)
            return True
        print(f"[Supabase] FAIL: {event} HTTP={code}", flush=True)
        return False
    except Exception as e:
        print(f"[Supabase] ERROR: {event} — {e}", flush=True)
        return False


def broadcast_now(event, payload):
    threading.Thread(
        target=_broadcast_via_rest,
        args=(event, dict(payload)),
        daemon=True,
        name=f"bc-{event}",
    ).start()

# ── Supabase WebSocket (keeps channel alive for JS subscribers) ───────────────

_realtime_client  = None
_realtime_channel = None
_rt_loop          = None
_rt_ready         = threading.Event()


async def ensure_realtime_connected():
    global _realtime_client, _realtime_channel
    try:
        if _realtime_client is None:
            ws_url = (
                SUPABASE_URL.replace("https://", "wss://")
                + "/realtime/v1/websocket"
            )
            _realtime_client = AsyncRealtimeClient(
                ws_url,
                token=SUPABASE_ANON_KEY,
                params={"apikey": SUPABASE_ANON_KEY},
            )
            await _realtime_client.connect()
            print("[Supabase] WebSocket connected")
        if _realtime_channel is None:
            _realtime_channel = _realtime_client.channel(CHANNEL_NAME)
            await _realtime_channel.subscribe()
            print(f"[Supabase] Subscribed to '{CHANNEL_NAME}'")
            _rt_ready.set()
    except Exception as e:
        print(f"[Supabase] Connect error: {e}")
        _realtime_client  = None
        _realtime_channel = None
        _rt_ready.clear()


async def realtime_loop():
    global _rt_loop
    _rt_loop = asyncio.get_running_loop()
    print("[Supabase] Realtime loop started")
    await ensure_realtime_connected()
    while True:
        await asyncio.sleep(3600)
        await ensure_realtime_connected()


def start_realtime_thread():
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.run_until_complete(realtime_loop())

# ── BLE state ─────────────────────────────────────────────────────────────────

_ble_connected      = False
_ble_connected_lock = threading.Lock()


def set_ble_connected(connected):
    global _ble_connected
    with _ble_connected_lock:
        if _ble_connected == connected:
            return
        _ble_connected = connected
    if connected:
        print("[BLE] Connected!")
        broadcast_now("status", {"connected": True})
    else:
        print("[BLE] Disconnected")

# ── BLE notification handler ──────────────────────────────────────────────────

_ble_buffer = ""


def handle_notification(sender, data):
    global _ble_buffer
    _ble_buffer += data.decode("utf-8", errors="replace")
    while True:
        start = _ble_buffer.find("{")
        end   = _ble_buffer.find("}", start)
        if start == -1 or end == -1:
            break
        raw         = _ble_buffer[start:end + 1]
        _ble_buffer = _ble_buffer[end + 1:]
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            continue
        msg_type = msg.get("t")
        if msg_type == "hike":
            steps    = msg.get("stp", 0)
            distance = msg.get("dst", 0)
            duration = msg.get("dur", 0)
            print(f"[BLE] live stp={steps} dst={distance}m dur={duration}s")
            update_live_cache(steps, distance, duration, source="ble")
            broadcast_now("live_update", {
                "source": "ble", "steps": steps,
                "distance": distance, "duration": duration,
                "calories": _calories(steps),
            })
        elif msg_type == "hike_end":
            handle_session_end(
                msg.get("stp", 0), msg.get("dst", 0), msg.get("dur", 0),
                source="ble",
            )

# ── BLE loop ──────────────────────────────────────────────────────────────────
# Uses hardcoded MAC address — no scan needed, connects directly.
# "failed to discover services" = watch GATT not ready yet.
# Fix: wait 3s after connect before subscribing to notifications.

BLE_CONNECT_TIMEOUT = 20.0
BLE_RETRY_DELAY     = 15.0   # wait between reconnect attempts


async def ble_loop():
    from bleak import BleakClient

    print(f"[BLE] Will connect to {WATCH_ADDRESS} when in range...")
    while True:
        client = None
        try:
            print(f"[BLE] Connecting to {WATCH_ADDRESS}...")
            async with BleakClient(
                WATCH_ADDRESS,
                timeout=BLE_CONNECT_TIMEOUT,
            ) as client:
                set_ble_connected(True)
                # Wait 3s for watch GATT stack to fully initialize
                # before trying to discover services
                await asyncio.sleep(3.0)
                await client.start_notify(UART_TX_CHAR_UUID, handle_notification)
                print("[BLE] Listening for packets...")
                while client.is_connected:
                    await asyncio.sleep(1)

        except asyncio.CancelledError:
            raise
        except Exception as e:
            if str(e):
                print(f"[BLE] {type(e).__name__}: {e}")
        finally:
            still_connected = False
            try:
                still_connected = client is not None and client.is_connected
            except Exception:
                pass
            if not still_connected:
                set_ble_connected(False)

        print(f"[BLE] Retry in {BLE_RETRY_DELAY:.0f}s...")
        await asyncio.sleep(BLE_RETRY_DELAY)


def start_ble_thread():
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.run_until_complete(ble_loop())

# ── Flask payload helpers ─────────────────────────────────────────────────────

def parse_payload():
    data = request.get_json(silent=True)
    if isinstance(data, dict):
        return data
    raw = request.data.decode("utf-8", errors="ignore").strip()
    if raw:
        try:
            parsed = json.loads(raw)
            if isinstance(parsed, dict):
                return parsed
        except Exception:
            pass
    if request.form:
        return request.form.to_dict()
    if request.args:
        return request.args.to_dict()
    return {}


def extract_fields(data):
    steps    = int(data.get("steps",    data.get("stp", 0)))
    distance = int(data.get("distance", data.get("dst", 0)))
    duration = int(data.get("duration", data.get("dur", 0)))
    return steps, distance, duration

# ── Flask endpoints ───────────────────────────────────────────────────────────

@app.route("/live", methods=["POST"])
def live():
    data = parse_payload()
    try:
        steps, distance, duration = extract_fields(data)
    except (TypeError, ValueError):
        return jsonify({"ok": False, "error": "Invalid payload"}), 400
    print(f"[WiFi] live stp={steps} dst={distance}m dur={duration}s")
    update_live_cache(steps, distance, duration, source="wifi")
    broadcast_now("live_update", {
        "source": "wifi", "steps": steps,
        "distance": distance, "duration": duration,
        "calories": _calories(steps),
    })
    return jsonify({"ok": True}), 200


@app.route("/session-end", methods=["POST"])
def session_end():
    data = parse_payload()
    try:
        steps, distance, duration = extract_fields(data)
    except (TypeError, ValueError):
        return jsonify({"ok": False, "error": "Invalid payload"}), 400
    if duration == 0:
        return jsonify({"ok": False, "error": "Zero duration"}), 400
    session = handle_session_end(steps, distance, duration, source="wifi")
    return jsonify({
        "ok": True,
        "steps":    session["steps"],
        "distance": session["distance"],
        "duration": session["duration"],
        "calories": session["calories"],
        "ended_at": session["ended_at"],
    }), 200


@app.route("/last-session")
def last_session():
    session = load_last_session()
    if session is None:
        return jsonify({"error": "No session recorded yet"}), 404
    return jsonify(session)


@app.route("/history")
def history():
    return jsonify(load_history())


@app.route("/current")
def current():
    return jsonify({
        "live":         get_live_cache(),
        "last_session": load_last_session(),
        "history":      load_history(),
    })


@app.route("/test-broadcast")
def test_broadcast():
    ok = _broadcast_via_rest("live_update", {
        "source": "test", "steps": 999,
        "distance": 500, "duration": 60, "calories": 39,
    })
    return jsonify({"ok": ok}), 200


@app.route("/status")
def status():
    return jsonify({
        "pi_running":     True,
        "ip":             get_local_ip(),
        "ble_connected":  _ble_connected,
        "supabase_ready": _rt_ready.is_set(),
        "last_session":   load_last_session(),
    })

# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    print("[Pi] Starting...")
    hist = load_history()
    last = load_last_session()
    print(f"[Pi] Last session : {last or 'none'}")
    print(f"[Pi] History      : {len(hist)} session(s)")

    zeroconf = start_mdns()

    rt_thread = threading.Thread(
        target=start_realtime_thread, daemon=True, name="supabase-rt")
    rt_thread.start()

    ble_thread = threading.Thread(
        target=start_ble_thread, daemon=True, name="ble")
    ble_thread.start()

    def _log_rt():
        if _rt_ready.wait(timeout=20.0):
            print("[Pi] Supabase ready")
        else:
            print("[Pi] Supabase timeout — check internet connection")
    threading.Thread(target=_log_rt, daemon=True).start()

    print(f"[Pi] Flask on 0.0.0.0:{SERVER_PORT}")
    app.run(host="0.0.0.0", port=SERVER_PORT, debug=False)