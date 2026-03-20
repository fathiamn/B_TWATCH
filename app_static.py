import asyncio
import json
import os
import threading
from datetime import datetime
from pathlib import Path

import httpx
from flask import Flask, jsonify, send_from_directory

# ─── Config ───────────────────────────────────────────────────────────────────

BASE_DIR = Path(__file__).resolve().parent

SUPABASE_URL = os.environ.get(
    "SUPABASE_URL",
    "https://sudlejmejjlairgxdlzi.supabase.co",
)
SUPABASE_ANON_KEY = os.environ.get(
    "SUPABASE_ANON_KEY",
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InN1ZGxlam1lampsYWlyZ3hkbHppIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzM1Nzc5OTIsImV4cCI6MjA4OTE1Mzk5Mn0.NRQy1vGT3LnbO1oo_yDoeVjOxz4xL9ErscJWNT1bAQoE",
)
CHANNEL_NAME = "twatch-activity"

WATCH_NAME = "Espruino (T-Watch2020V2)"
UART_TX_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

SESSION_JSON = BASE_DIR / "last_session.json"
SESSION_TXT = BASE_DIR / "last_session.txt"
DASHBOARD_HTML = BASE_DIR / "dashboard.html"

# ─── Flask ────────────────────────────────────────────────────────────────────

app = Flask(__name__, static_folder=str(BASE_DIR), static_url_path="")

@app.after_request
def add_cors_headers(response):
    response.headers["Access-Control-Allow-Origin"] = "*"
    response.headers["Access-Control-Allow-Headers"] = "Content-Type"
    response.headers["Access-Control-Allow-Methods"] = "GET, OPTIONS"
    return response

@app.route("/")
def serve_dashboard():
    if DASHBOARD_HTML.exists():
        return send_from_directory(str(BASE_DIR), "dashboard.html")
    return jsonify({"error": "dashboard.html not found"}), 404

@app.route("/dashboard.html")
def serve_dashboard_file():
    if DASHBOARD_HTML.exists():
        return send_from_directory(str(BASE_DIR), "dashboard.html")
    return jsonify({"error": "dashboard.html not found"}), 404

# ─── File storage ─────────────────────────────────────────────────────────────

def save_last_session(steps: int, distance: int, duration: int) -> dict:
    calories = round(steps * 0.04, 1)
    ended_at = datetime.now().isoformat(timespec="seconds")

    data = {
        "steps": int(steps),
        "distance": int(distance),
        "duration": int(duration),
        "calories": calories,
        "ended_at": ended_at,
    }

    try:
        with open(SESSION_JSON, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
        print(f"[File] Saved JSON → {SESSION_JSON}")
    except Exception as e:
        print(f"[File] JSON save error: {e}")

    try:
        mins = duration // 60
        secs = duration % 60
        with open(SESSION_TXT, "w", encoding="utf-8") as f:
            f.write("=== Last Hiking Session ===\n")
            f.write(f"Date/Time : {ended_at}\n")
            f.write(f"Steps     : {steps}\n")
            f.write(f"Distance  : {distance} m\n")
            f.write(f"Duration  : {mins}m {secs}s\n")
            f.write(f"Calories  : {calories} kcal\n")
        print(f"[File] Saved TXT  → {SESSION_TXT}")
    except Exception as e:
        print(f"[File] TXT save error: {e}")

    return data

def load_last_session() -> dict | None:
    try:
        with open(SESSION_JSON, "r", encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return None
    except Exception as e:
        print(f"[File] Load error: {e}")
        return None

# ─── Supabase broadcast ───────────────────────────────────────────────────────

def supabase_broadcast(event: str, payload: dict):
    if not SUPABASE_URL or not SUPABASE_ANON_KEY or SUPABASE_ANON_KEY == "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InN1ZGxlam1lampsYWlyZ3hkbHppIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzM1Nzc5OTIsImV4cCI6MjA4OTE1Mzk5Mn0.NRQy1vGT3LnbO1oo_yDoeVjOxz4xL9ErscJWNT1bAQo":
        print("[Supabase] Skipped broadcast: missing SUPABASE_URL or SUPABASE_ANON_KEY")
        return

    url = f"{SUPABASE_URL}/realtime/v1/api/broadcast"
    headers = {
        "apikey": SUPABASE_ANON_KEY,
        "Authorization": f"Bearer {SUPABASE_ANON_KEY}",
        "Content-Type": "application/json",
    }
    body = {
        "messages": [
            {
                "topic": f"realtime:{CHANNEL_NAME}",
                "event": event,
                "payload": payload,
            }
        ]
    }

    try:
        response = httpx.post(url, headers=headers, json=body, timeout=4.0)
        if response.status_code not in (200, 202):
            print(f"[Supabase] Broadcast failed {response.status_code}: {response.text}")
        else:
            print(f"[Supabase] Broadcast OK event={event}")
    except Exception as e:
        print(f"[Supabase] Broadcast error: {e}")

def broadcast_async(event: str, payload: dict):
    threading.Thread(
        target=supabase_broadcast,
        args=(event, dict(payload)),
        daemon=True,
    ).start()

# ─── BLE notification handler ─────────────────────────────────────────────────

_ble_buffer = ""

def handle_notification(sender, data: bytearray):
    global _ble_buffer

    _ble_buffer += data.decode("utf-8", errors="replace")

    while True:
        start = _ble_buffer.find("{")
        end = _ble_buffer.find("}", start)

        if start == -1 or end == -1:
            break

        raw = _ble_buffer[start:end + 1]
        _ble_buffer = _ble_buffer[end + 1:]

        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            print(f"[BLE] Bad JSON skipped: {raw!r}")
            continue

        msg_type = msg.get("t")

        if msg_type == "hike":
            steps = int(msg.get("stp", 0))
            distance = int(msg.get("dst", 0))
            duration = int(msg.get("dur", 0))

            payload = {
                "type": "live",
                "steps": steps,
                "distance": distance,
                "duration": duration,
                "calories": round(steps * 0.04),
            }

            print(f"[BLE] live hike → stp={steps} dst={distance}m dur={duration}s")
            broadcast_async("live_update", payload)

        elif msg_type == "hike_end":
            steps = int(msg.get("stp", 0))
            distance = int(msg.get("dst", 0))
            duration = int(msg.get("dur", 0))

            print(f"[BLE] hike_end → stp={steps} dst={distance}m dur={duration}s")

            session = save_last_session(steps, distance, duration)

            payload = {
                "type": "session_end",
                "steps": session["steps"],
                "distance": session["distance"],
                "duration": session["duration"],
                "calories": session["calories"],
                "ended_at": session["ended_at"],
            }

            broadcast_async("session_end", payload)

        elif msg_type == "act":
            delta = int(msg.get("stp", 0))
            print(f"[BLE] act delta={delta}")

        else:
            print(f"[BLE] Unknown message type: {msg_type} → {msg}")

# ─── BLE scanner loop ─────────────────────────────────────────────────────────

async def ble_loop():
    from bleak import BleakClient, BleakScanner

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

@app.route("/status")
def status():
    session = load_last_session()
    return jsonify(
        {
            "pi_running": True,
            "dashboard_exists": DASHBOARD_HTML.exists(),
            "last_session": session,
        }
    )

# ─── Entry point ──────────────────────────────────────────────────────────────

if __name__ == "__main__":
    print("[Pi] T-Watch Hiking Band server starting...")
    print(f"[Pi] Base dir:       {BASE_DIR}")
    print(f"[Pi] JSON file:      {SESSION_JSON}")
    print(f"[Pi] TXT file:       {SESSION_TXT}")
    print(f"[Pi] Dashboard file: {DASHBOARD_HTML}")
    print(f"[Pi] Supabase URL:   {SUPABASE_URL}")
    print(f"[Pi] Channel:        {CHANNEL_NAME}")

    last = load_last_session()
    if last:
        print(f"[Pi] Last session: {last}")
    else:
        print("[Pi] No previous session found")

    ble_thread = threading.Thread(target=start_ble_thread, daemon=True)
    ble_thread.start()

    app.run(host="0.0.0.0", port=5000, debug=False)