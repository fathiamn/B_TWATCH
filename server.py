# © 2026 Monterro · Fathia & Bintang. All rights reserved.

from __future__ import annotations

import asyncio
import json
import logging
import threading
import time
from collections import deque
from datetime import datetime, timezone
from typing import Optional

import httpx

try:
    from bleak import BleakClient, BleakScanner
    from bleak.backends.characteristic import BleakGATTCharacteristic
    BLE_AVAILABLE = True
except ImportError:
    BLE_AVAILABLE = False

try:
    import pymysql
    MYSQL_AVAILABLE = True
except ImportError:
    MYSQL_AVAILABLE = False

# Configuration

SUPABASE_URL       = "https://sudlejmejjlairgxdlzi.supabase.co"
SUPABASE_KEY       = (
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"
    ".eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InN1ZGxlam1lampsYWlyZ3hkbHppIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzM1Nzc5OTIsImV4cCI6MjA4OTE1Mzk5Mn0"
    ".NRQy1vGT3LnbO1oo_yDoeVjOxz4xL9ErscJWNT1bAQo"
)
CHANNEL_NAME       = "twatch-activity"
WATCH_NAME         = "Espruino (T-Watch2020V2)"
WATCH_ADDRESS      = "08:3A:F2:69:AA:96"
UART_TX_CHAR_UUID  = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

DB_HOST            = "localhost"
DB_NAME            = "monterro"
DB_USER            = "monterro"
DB_PASS            = "monterro_pass"

HISTORY_MAX        = 10
HEARTBEAT_INTERVAL = 5    # seconds
SESSION_TIMEOUT_S  = 30   # seconds of silence before session expired


logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("monterro")

if not BLE_AVAILABLE:
    log.warning("[BLE] bleak not installed — BLE disabled")
if not MYSQL_AVAILABLE:
    log.warning("[MySQL] pymysql not installed — DB sync disabled")

# Shared state

class State:

    def __init__(self) -> None:
        self._lock           = threading.Lock()
        self.session_active  : bool  = False
        self.steps           : int   = 0
        self.distance        : int   = 0
        self.duration        : int   = 0
        self.calories        : int   = 0
        self.watch_connected : bool  = False
        self.last_session    : dict  = {}
        self.history         : deque = deque(maxlen=HISTORY_MAX)
        self.last_live_ts    : float = 0.0
        self.watch_data_ts   : float = 0.0  # BLE only

    def update_live(self, steps: int, distance: int, duration: int) -> dict:
        """Called by BLE — stamps both timestamps, sets session_active."""
        cal = steps * 4 // 100
        now = time.monotonic()
        with self._lock:
            self.steps          = steps
            self.distance       = distance
            self.duration       = duration
            self.calories       = cal
            self.session_active = True
            self.last_live_ts   = now
            self.watch_data_ts  = now
        return {
            "steps": steps, "distance": distance,
            "duration": duration, "calories": cal,
            "session_active": True,
        }

    def sync_from_db(self, steps: int, distance: int,
                     duration: int, calories: int) -> None:
        with self._lock:
            self.steps    = steps
            self.distance = distance
            self.duration = duration
            self.calories = calories

    def end_session(self, steps: int, distance: int, duration: int,
                    history_from_watch: Optional[list] = None) -> dict:
        cal      = steps * 4 // 100
        ended_at = datetime.now(timezone.utc).isoformat()
        record   = {
            "steps": steps, "distance": distance,
            "duration": duration, "calories": cal, "ended_at": ended_at,
        }
        with self._lock:
            self.steps          = steps
            self.distance       = distance
            self.duration       = duration
            self.calories       = cal
            self.session_active = False
            self.last_session   = record
            if history_from_watch:
                normalised = []
                for e in history_from_watch[:HISTORY_MAX]:
                    entry: dict = {
                        "steps"   : int(e.get("steps",    0)),
                        "distance": int(e.get("distance", 0)),
                        "duration": int(e.get("duration", 0)),
                        "calories": int(e.get("calories", 0)),
                    }
                    if "ended_at" in e:
                        entry["ended_at"] = e["ended_at"]
                    normalised.append(entry)
                self.history = deque(normalised, maxlen=HISTORY_MAX)
            else:
                self.history.appendleft(record)
            snapshot = list(self.history)
        return {**record, "history": snapshot}

    def set_connected(self, connected: bool) -> None:
        with self._lock:
            self.watch_connected = connected

    def snapshot(self) -> dict:
        with self._lock:
            return {
                "live": {
                    "steps"         : self.steps,
                    "distance"      : self.distance,
                    "duration"      : self.duration,
                    "calories"      : self.calories,
                    "session_active": self.session_active,
                },
                "last_session": self.last_session or {"error": "no session yet"},
                "history"     : list(self.history),
                "connected"   : self.watch_connected,
            }


state = State()

# Supabase broadcast

_BROADCAST_URL = (
    f"{SUPABASE_URL}/realtime/v1/api/broadcast?apikey={SUPABASE_KEY}"
)
_BROADCAST_HEADERS = {
    "Content-Type" : "application/json",
    "apikey"       : SUPABASE_KEY,
    "Authorization": f"Bearer {SUPABASE_KEY}",
}


def broadcast(event: str, payload: dict) -> None:
    body = {"messages": [{
        "topic"  : f"realtime:{CHANNEL_NAME}",
        "event"  : event,
        "payload": payload,
    }]}
    try:
        with httpx.Client(timeout=4) as client:
            r = client.post(_BROADCAST_URL, json=body, headers=_BROADCAST_HEADERS)
        if r.status_code not in (200, 202):
            log.warning("[Supabase] %s → %d  %s", event, r.status_code, r.text[:120])
        else:
            log.info("[Supabase] %-15s → %d", event, r.status_code)
    except Exception as e:
        log.error("[Supabase] %s failed: %s", event, e)


def broadcast_async(event: str, payload: dict) -> None:
    threading.Thread(target=broadcast, args=(event, payload), daemon=True).start()

# Data Flow Handlers (called by BLE notifications)

def handle_live_update(steps: int, distance: int, duration: int) -> None:
    log.info("[BLE] live  steps=%-5d dist=%-4dm dur=%ds", steps, distance, duration)
    broadcast_async("live_update", state.update_live(steps, distance, duration))


def handle_session_end(steps: int, distance: int, duration: int,
                       history_from_watch: Optional[list] = None) -> None:
    log.info("[BLE] END   steps=%-5d dist=%-4dm dur=%ds  history=%d",
             steps, distance, duration,
             len(history_from_watch) if history_from_watch else 0)
    broadcast_async("session_end",
                    state.end_session(steps, distance, duration, history_from_watch))


def handle_watch_connected(connected: bool) -> None:
    prev = state.watch_connected
    state.set_connected(connected)
    if connected != prev:
        log.info("[BLE] connected=%s", connected)
        broadcast_async("status", {"connected": connected})

# Heartbeat

def _heartbeat_tick() -> None:
    snap = state.snapshot()

    # Auto-expire stale session
    if snap["live"]["session_active"]:
        silence = time.monotonic() - state.last_live_ts
        if silence > SESSION_TIMEOUT_S:
            with state._lock:
                state.session_active = False
            log.warning("[heartbeat] session expired after %.0fs silence", silence)
            snap = state.snapshot()

    # Status broadcast — always
    broadcast("status", {"connected": snap["connected"]})
    watch_age = time.monotonic() - state.watch_data_ts
    if snap["live"]["session_active"] and watch_age < SESSION_TIMEOUT_S:
        broadcast("live_update", snap["live"]) # only live data when session active and recent BLE data
        log.info("[heartbeat] live_update  steps=%d  dist=%dm  dur=%ds",
                 snap["live"]["steps"], snap["live"]["distance"],
                 snap["live"]["duration"])
    else:
        log.debug("[heartbeat] idle  watch_age=%.0fs", watch_age)


def _heartbeat_loop() -> None:
    while True:
        time.sleep(HEARTBEAT_INTERVAL)
        try:
            _heartbeat_tick()
        except Exception as e:
            log.error("[heartbeat] crashed: %s", e, exc_info=True)


def start_heartbeat_thread() -> None:
    threading.Thread(target=_heartbeat_loop, name="heartbeat", daemon=True).start()
    log.info("[heartbeat] started (interval=%ds)", HEARTBEAT_INTERVAL)

def sync_state_from_mysql() -> None:
    """
    Read latest live_data row from MySQL.
    Only updates steps/distance/duration/calories if the row is fresh.
    Never sets session_active, last_live_ts, or watch_data_ts.
    """
    if not MYSQL_AVAILABLE:
        return
    try:
        conn = pymysql.connect(
            host=DB_HOST, user=DB_USER, password=DB_PASS,
            database=DB_NAME, connect_timeout=3,
        )
        with conn.cursor() as cur:
            cur.execute(
                "SELECT steps, distance, duration, calories, "
                "TIMESTAMPDIFF(SECOND, received_at, NOW()) AS age_s "
                "FROM live_data ORDER BY id DESC LIMIT 1"
            )
            row = cur.fetchone()
            if row:
                steps, distance, duration, calories, age_s = row
                if age_s is not None and age_s < SESSION_TIMEOUT_S:
                    state.sync_from_db(int(steps), int(distance),
                                       int(duration), int(calories))

            cur.execute(
                "SELECT steps, distance, duration, calories, ended_at "
                "FROM sessions ORDER BY id DESC LIMIT 1"
            )
            row = cur.fetchone()
            if row and not state.last_session:
                state.last_session = {
                    "steps"   : int(row[0]),
                    "distance": int(row[1]),
                    "duration": int(row[2]),
                    "calories": int(row[3]),
                    "ended_at": str(row[4]),
                }
        conn.close()
    except Exception as e:
        log.debug("[MySQL] sync error: %s", e)

# BLE — Nordic UART Service (NUS)

_nus_buffer = ""
_nus_lock   = threading.Lock()


def _reset_nus_buffer() -> None:
    global _nus_buffer
    with _nus_lock:
        _nus_buffer = ""


def _nus_dispatch(msg: dict) -> None:
    mtype = msg.get("type", "")
    if mtype == "live":
        handle_live_update(
            int(msg.get("steps",    0)),
            int(msg.get("distance", 0)),
            int(msg.get("duration", 0)),
        )
    elif mtype == "end":
        raw = msg.get("history")
        handle_session_end(
            int(msg.get("steps",    0)),
            int(msg.get("distance", 0)),
            int(msg.get("duration", 0)),
            history_from_watch=raw if isinstance(raw, list) else None,
        )
    elif mtype == "status":
        handle_watch_connected(bool(msg.get("connected", False)))
    else:
        log.warning("[BLE] unknown msg type: %r", mtype)


def ble_notification_handler(char: "BleakGATTCharacteristic",
                              data: bytearray) -> None:
    global _nus_buffer
    chunk = data.decode("utf-8", errors="replace")
    with _nus_lock:
        _nus_buffer += chunk
        lines: list[str] = []
        while "\n" in _nus_buffer:
            line, _nus_buffer = _nus_buffer.split("\n", 1)
            lines.append(line.strip())
    for line in lines:
        if not line:
            continue
        try:
            _nus_dispatch(json.loads(line))
        except json.JSONDecodeError:
            log.warning("[BLE] invalid JSON: %r", line)


async def ble_run_forever() -> None:
    if not BLE_AVAILABLE:
        return
    log.info("[BLE] target: %s  %s", WATCH_ADDRESS, WATCH_NAME)
    retry_delay = 5

    while True:
        _reset_nus_buffer()
        was_connected = state.watch_connected

        log.info("[BLE] scanning (timeout 15s)…")
        try:
            device = await BleakScanner.find_device_by_address(
                WATCH_ADDRESS, timeout=15.0
            )
        except Exception as e:
            log.warning("[BLE] scan error: %s", e)
            device = None

        if device is None:
            wifi_age = time.monotonic() - state.last_live_ts
            if wifi_age < 30:
                log.info("[BLE] WiFi active (%.0fs ago) — backing off 60s", wifi_age)
                await asyncio.sleep(60)
                continue
            log.warning("[BLE] not found — retry in %ds", retry_delay)
            if was_connected:
                handle_watch_connected(False)
            retry_delay = min(retry_delay * 2, 60)
            await asyncio.sleep(retry_delay)
            continue

        try:
            async with BleakClient(device, timeout=15.0) as client:
                if not client.is_connected:
                    raise ConnectionError("connect returned disconnected")
                retry_delay = 5
                handle_watch_connected(True)
                log.info("[BLE] connected — subscribing %s", UART_TX_CHAR_UUID)
                await client.start_notify(UART_TX_CHAR_UUID, ble_notification_handler)
                log.info("[BLE] subscribed — waiting for data")
                while client.is_connected:
                    await asyncio.sleep(1)
        except Exception as e:
            msg = str(e).strip()
            log.warning("[BLE] %s", msg if msg else "error (GATT timeout)")

        handle_watch_connected(False)
        log.info("[BLE] disconnected — retry in %ds", retry_delay)
        await asyncio.sleep(retry_delay)
        retry_delay = min(retry_delay * 2, 60)


def start_ble_thread() -> None:
    if not BLE_AVAILABLE:
        log.info("[BLE] disabled")
        return

    def _run() -> None:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        loop.run_until_complete(ble_run_forever())

    threading.Thread(target=_run, name="ble-loop", daemon=True).start()
    log.info("[BLE] background thread started")

# Entry point (starts BLE thread, heartbeat thread, and MySQL sync loop)

if __name__ == "__main__":
    log.info("═" * 60)
    log.info("  Monterro Pi Bridge")
    log.info("  WiFi : Watch → Apache → PHP → MySQL + Supabase")
    log.info("  BLE  : Watch → server.py → Supabase")
    log.info("  Channel  : %s", CHANNEL_NAME)
    log.info("  Watch    : %s  %s", WATCH_NAME, WATCH_ADDRESS)
    log.info("  BLE      : %s", BLE_AVAILABLE)
    log.info("  MySQL    : %s", MYSQL_AVAILABLE)
    log.info("═" * 60)

    sync_state_from_mysql()
    start_ble_thread()
    start_heartbeat_thread()

    try:
        while True:
            time.sleep(HEARTBEAT_INTERVAL)
            sync_state_from_mysql()
    except KeyboardInterrupt:
        log.info("Shutting down")
