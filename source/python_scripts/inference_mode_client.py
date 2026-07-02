import asyncio
import sys
from bleak import BleakScanner, BleakClient

TARGET_DEVICE_NAME = "ESP32-S3-EMG"
GESTURE_CHARACTERISTIC_UUID = "12345678-1234-1234-5678-123412345678"

GESTURE_NAMES = ["Relax", "Fingersnap", "Fist"]

def gesture_notification_handler(sender, data):
    if len(data) < 3:
        return

    relax, snap, fist = data[:3]

    print(
        f"Relax: {relax:3}% | "
        f"Fingersnap:  {snap:3}% | "
        f"Fist:  {fist:3}%",
    )

async def main():
    print(f"Searching for {TARGET_DEVICE_NAME}...")
    try:
        device = await BleakScanner.find_device_by_filter(
            lambda d, ad: d.name == TARGET_DEVICE_NAME
        )
    except Exception as e:
        print(e.args[0])
        return

    if not device:
        print("Device not found!")
        return

    print(f"Connecting to {device.address}...")
    async with BleakClient(device) as client:
        print("Connected!")

        try:
            await client.start_notify(GESTURE_CHARACTERISTIC_UUID, gesture_notification_handler)
            print("Waiting for notifications!")
        except Exception as e:
            print(e.args[0])
            return

        
        stop_event = asyncio.Event()
        await stop_event.wait()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nKeyboard interrupt")