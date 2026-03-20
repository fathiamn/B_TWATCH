"""
T-Watch Hiking Band — Pi BLE Receiver
--------------------------------------
- Receives live "hike" packets   → broadcasts to Supabase (live dashboard)
- Receives "hike_end" packet     → saves to last_session.json + last_session.txt
                                   appends to session_history.json (last 10)
                                   broadcasts session_end WITH full history list
- Serves /last-session endpoint  → dashboard fetches on page load
- Serves /history endpoint       → dashboard fetches full history on page load
- Serves /status endpoint        → health check

Install:
    pip install flask flask-cors bleak realtime --break-system-packages

Run:
    python app.py
"""

import asyncio
import json
import os
import threading
from datetime import datetime

from flask import Flask, jsonify
from flask_cors import CORS          # ← NEW: pip install flask-cors

# ─── Config ───────────────────────────────────────────────────────────────────

SUPABASE_URL      = os.environ.get("SUPABASE_URL",      "https://sudlejmejjlairgxdlzi.supabase.co")
SUPABASE_ANON_KEY = os.environ.get("SUPABASE_ANON_KEY", "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InN1ZGxlam1lampsYWlyZ3hkbHppIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzM1Nzc5OTIsImV4cCI6MjA4OTE1Mzk5Mn0.NRQy1vGT3LnbO1oo_yDoeVjOxz4xL9ErscJWNT1bAQo")
CHANNEL_NAME      = "twatch-activity"

WATCH_NAME        = "Espruino (T-Watch2020V2)"
UART_TX_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

SESSION_JSON  = "last_session.json"
SESSION_TXT   = "last_session.txt"
HISTORY_JSON  = "session_history.json"
HISTORY_MAX   = 10

# ─── Flask ────────────────────────────────────────────────────────────────────

app = Flask(__name__)

# ── CORS: allow any origin so Vercel (and any browser) can call the Pi ────────
# origins="*" is fine here because the Pi only serves read-only session data.
# If you want to lock it to just your Vercel domain replace with:
#   origins=["https://monterro.vercel.app"]
CORS(app, origins="*")

# ─── File storage ─────────────────────────────────────────────────────────────

def save_last_session(steps: int, distance: int, duration: int) -> dict:
    calories = round(steps * 0.04, 1)
    ended_at = datetime.now().isoformat(timespec="seconds")

    data = {
        "steps":    steps,
        "distance": distance,
        "duration": duration,
        "calories": calories,
        "ended_at": ended_at,
    }

    try:
        with open(SESSION_JSON, "w") as f:
            json.dump(data, f, indent=2)
        print(f"[File] Saved {SESSION_JSON} → {data}")
    except Exception as e:
        print(f"[File] JSON save error: {e}")

    try:
        mins = duration // 60
        secs = duration % 60
        with open(SESSION_TXT, "w") as f:
            f.write("=== Last Hiking Session ===\n")
            f.write(f"Date/Time : {ended_at}\n")
            f.write(f"Steps     : {steps}\n")
            f.write(f"Distance  : {distance} m\n")
            f.write(f"Duration  : {mins}m {secs}s\n")
            f.write(f"Calories  : {calories} kcal\n")
        print(f"[File] Saved {SESSION_TXT}")
    except Exception as e:
        print(f"[File] TXT save error: {e}")

    return data


def load_last_session() -> dict | None:
    try:
        with open(SESSION_JSON, "r") as f:
            return json.load(f)
    except FileNotFoundError:
        return None
    except Exception as e:
        print(f"[File] Load error: {e}")
        return None


def load_history() -> list:
    try:
        with open(HISTORY_JSON, "r") as f:
            data = json.load(f)
            if isinstance(data, list):
                return data
    except FileNotFoundError:
        pass
    except Exception as e:
        print(f"[File] History load error: {e}")
    return []


def save_history(history: list):
    try:
        with open(HISTORY_JSON, "w") as f:
            json.dump(history, f, indent=2)
        print(f"[File] Saved {HISTORY_JSON} ({len(history)} sessions)")
    except Exception as e:
        print(f"[File] History save error: {e}")


def append_session_to_history(session: dict) -> list:
    history = load_history()
    entry = {
        "steps":    session.get("steps",    0),
        "distance": session.get("distance", 0),
        "duration": session.get("duration", 0),
        "calories": session.get("calories", 0),
        "ended_at": session.get("ended_at", datetime.now().isoformat(timespec="seconds")),
    }
    history.insert(0, entry)
    history = history[:HISTORY_MAX]
    save_history(history)
    return history

# ─── Supabase Realtime ────────────────────────────────────────────────────────

from realtime import AsyncRealtimeClient

_realtime_client  = None
_realtime_channel = None
_ble_loop         = None


async def ensure_realtime_connected():
    global _realtime_client, _realtime_channel
    try:
        if _realtime_client is None:
            ws_url = SUPABASE_URL.replace("https://", "wss://") + "/realtime/v1/websocket"
            _realtime_client = AsyncRealtimeClient(
                ws_url,
                token=SUPABASE_ANON_KEY,
                params={"apikey": SUPABASE_ANON_KEY}
            )
            await _realtime_client.connect()
            _realtime_channel = _realtime_client.channel(CHANNEL_NAME)
            await _realtime_channel.subscribe()
            print(f"[Supabase] Connected to channel '{CHANNEL_NAME}'")
    except Exception as e:
        print(f"[Supabase] Connect error: {e}")
        _realtime_client  = None
        _realtime_channel = None


async def supabase_broadcast_async(event: str, payload: dict):
    global _realtime_client, _realtime_channel
    try:
        await ensure_realtime_connected()
        if _realtime_channel is None:
            print(f"[Supabase] No channel, skipping broadcast")
            return
        await _realtime_channel.send_broadcast(event, payload)
        print(f"[Supabase] Broadcast OK event={event} → {payload}")
    except Exception as e:
        print(f"[Supabase] Broadcast error: {e}")
        _realtime_client  = None
        _realtime_channel = None


def broadcast_async(event: str, payload: dict):
    if _ble_loop is not None:
        asyncio.run_coroutine_threadsafe(
            supabase_broadcast_async(event, dict(payload)),
            _ble_loop
        )
    else:
        print(f"[Supabase] Event loop not ready, skipping broadcast")

# ─── BLE notification handler ─────────────────────────────────────────────────

_ble_buffer = ""

def handle_notification(sender, data: bytearray):
    global _ble_buffer
    _ble_buffer += data.decode("utf-8", errors="replace")

    while True:
        start = _ble_buffer.find("{")
        end   = _ble_buffer.find("}", start)
        if start == -1 or end == -1:
            break

        raw         = _ble_buffer[start : end + 1]
        _ble_buffer = _ble_buffer[end + 1:]

        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            print(f"[BLE] Bad JSON skipped: {raw!r}")
            continue

        msg_type = msg.get("t")

        if msg_type == "hike":
            steps    = msg.get("stp", 0)
            distance = msg.get("dst", 0)
            duration = msg.get("dur", 0)
            print(f"[BLE] live hike → stp={steps} dst={distance}m dur={duration}s")
            broadcast_async("live_update", {
                "type": "live", "steps": steps,
                "distance": distance, "duration": duration,
            })

        elif msg_type == "hike_end":
            steps    = msg.get("stp", 0)
            distance = msg.get("dst", 0)
            duration = msg.get("dur", 0)
            print(f"[BLE] hike_end → stp={steps} dst={distance}m dur={duration}s")

            session = save_last_session(steps, distance, duration)
            history = append_session_to_history(session)

            broadcast_async("session_end", {
                "type":     "session_end",
                "steps":    session["steps"],
                "distance": session["distance"],
                "duration": session["duration"],
                "calories": session["calories"],
                "ended_at": session["ended_at"],
                "history":  list(reversed(history)),   # oldest-first for graph
            })

        elif msg_type == "act":
            print(f"[BLE] act delta={msg.get('stp', 0)}")

# ─── BLE async loop ───────────────────────────────────────────────────────────

async def ble_loop():
    global _ble_loop
    from bleak import BleakScanner, BleakClient

    _ble_loop = asyncio.get_event_loop()
    print("[Supabase] Connecting to Realtime...")
    await ensure_realtime_connected()

    print("[BLE] Scanning for watch...")
    while True:
        try:
            device = await BleakScanner.find_device_by_name(WATCH_NAME, timeout=10.0)
            if device is None:
                print(f"[BLE] '{WATCH_NAME}' not found, retrying in 5s...")
                await asyncio.sleep(5)
                continue

            print(f"[BLE] Found {device.name} ({device.address}), connecting...")
            async with BleakClient(device) as client:
                print("[BLE] Connected!")
                broadcast_async("status", {"connected": True})
                await client.start_notify(UART_TX_CHAR_UUID, handle_notification)
                print("[BLE] Listening...")
                while client.is_connected:
                    await asyncio.sleep(1)

        except Exception as e:
            print(f"[BLE] Error: {e}")
        finally:
            broadcast_async("status", {"connected": False})
            print("[BLE] Disconnected. Retrying in 5s...")
            await asyncio.sleep(5)


def start_ble_thread():
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.run_until_complete(ble_loop())

# ─── Flask endpoints ──────────────────────────────────────────────────────────

@app.route("/last-session")
def last_session():
    session = load_last_session()
    if session is None:
        return jsonify({"error": "No session recorded yet"}), 404
    return jsonify(session)


@app.route("/history")
def history():
    data = load_history()                  # newest-first on disk
    return jsonify(list(reversed(data)))   # oldest-first for graph


@app.route("/status")
def status():
    return jsonify({
        "pi_running":   True,
        "last_session": load_last_session(),
    })

# ─── Entry point ──────────────────────────────────────────────────────────────

if __name__ == "__main__":
    print("[Pi] T-Watch Hiking Band server starting...")
    print(f"[Pi] JSON file:     {os.path.abspath(SESSION_JSON)}")
    print(f"[Pi] TXT file:      {os.path.abspath(SESSION_TXT)}")
    print(f"[Pi] History file:  {os.path.abspath(HISTORY_JSON)}")

    last = load_last_session()
    hist = load_history()
    print(f"[Pi] Last session: {last or 'none'}")
    print(f"[Pi] History: {len(hist)} session(s) on disk")

    ble_thread = threading.Thread(target=start_ble_thread, daemon=True)
    ble_thread.start()

    app.run(host="0.0.0.0", port=5000, debug=False)