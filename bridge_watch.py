import asyncio
import json
from bleak import BleakClient
import websockets

WATCH_MAC = "08:3A:F2:69:AA:96"  # your watch
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # notify: watch -> pi

latest = {"t": "none"}

clients = set()

def parse_lines(buf: bytearray):
    # messages are like \r\n{json}\r\n, can arrive split
    text = buf.decode("utf-8", errors="ignore")
    # return list of complete JSON-ish lines
    # keep it simple: split by newline and filter
    return [line.strip() for line in text.splitlines() if line.strip()]

async def ws_handler(ws):
    clients.add(ws)
    try:
        # send last-known immediately
        await ws.send(json.dumps(latest))
        await ws.wait_closed()
    finally:
        clients.discard(ws)

async def broadcast(obj):
    if not clients:
        return
    msg = json.dumps(obj)
    dead = []
    for ws in clients:
        try:
            await ws.send(msg)
        except:
            dead.append(ws)
    for ws in dead:
        clients.discard(ws)

async def ble_loop():
    global latest
    async with BleakClient(WATCH_MAC) as client:
        print("BLE connected:", client.is_connected)

        buf = bytearray()

        def on_notify(_sender, data: bytearray):
            nonlocal buf
            buf.extend(data)

        await client.start_notify(NUS_TX_UUID, on_notify)
        print("Listening notifications...")

        while True:
            # consume buffer periodically
            if buf:
                chunk = bytes(buf)
                buf.clear()

                for line in parse_lines(bytearray(chunk)):
                    # try JSON decode if it looks like JSON
                    if line.startswith("{") and line.endswith("}"):
                        try:
                            obj = json.loads(line)
                            latest = obj
                            # push to browser
                            asyncio.create_task(broadcast(obj))
                            print("RX:", obj)
                        except:
                            print("RX (non-json):", line)
                    else:
                        print("RX:", line)

            await asyncio.sleep(0.1)

async def main():
    ws_server = await websockets.serve(ws_handler, "0.0.0.0", 8765)
    print("WebSocket on ws://<pi-ip>:8765")
    try:
        await ble_loop()
    finally:
        ws_server.close()
        await ws_server.wait_closed()

if __name__ == "__main__":
    asyncio.run(main())