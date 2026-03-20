import asyncio
from bleak import BleakClient

WATCH_MAC = "08:3A:F2:69:AA:96"   # your watch MAC

# You'll fill this after discovery if needed:
NOTIFY_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

async def main():
    async with BleakClient(WATCH_MAC) as client:
        print("Connected:", client.is_connected)

        # Bleak 2.x: services are available via client.services
        svcs = list(client.services)
        print("Service count:", len(svcs))

        # Find a characteristic that supports notify
        notify_chars = []
        for service in svcs:
            for ch in service.characteristics:
                if "notify" in ch.properties:
                    notify_chars.append(ch.uuid)

        print("Notify characteristics:")
        for u in notify_chars:
            print("  ", u)

        # If you already know the notify UUID, use it.
        uuid = NOTIFY_CHAR_UUID

        def handler(sender, data: bytearray):
            try:
                txt = data.decode("utf-8", errors="ignore").strip()
            except:
                txt = str(data)
            if txt:
                print("RX:", txt)

        await client.start_notify(uuid, handler)
        print("Listening... Ctrl+C to stop")
        while True:
            await asyncio.sleep(1)

asyncio.run(main())
