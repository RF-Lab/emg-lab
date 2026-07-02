import asyncio
import struct
import csv
import time
from bleak import BleakScanner, BleakClient

DEVICE_NAME = "ESP32-S3-EMG"
RAWDATA_UUID = "12345678-1234-1234-5678-123412345677"
DURATION_SEC = 60
OUTPUT_FILE = "fist.csv"
UNPACK_FORMAT = '<I60i' 

dataset_buffer = []

def notification_handler(sender, data):
    try:
        unpacked = struct.unpack(UNPACK_FORMAT, data)
        dataset_buffer.append([time.time(), unpacked[0]] + list(unpacked[1:]))
    except Exception as e:
        pass

async def main():
    total_recorded_time = 0.0

    while total_recorded_time < DURATION_SEC:
        print(f"\nSearch for device '{DEVICE_NAME}'...")
        try:
            device = await BleakScanner.find_device_by_filter(
                lambda d, ad: d.name == DEVICE_NAME, 
                timeout=5.0
            )
        
        except Exception as e:
            print(e.args[0])
            return

        print(f"Device found: {device.address}. Connecting...")

        try:
            async with BleakClient(device, timeout=10.0) as client:
                print("Connected! Start data collection...")
                await client.start_notify(RAWDATA_UUID, notification_handler)
                
                last_tick = time.time()

                while total_recorded_time < DURATION_SEC:
                    if not client.is_connected:
                        print("\n[WARN] Data loss...")
                        break
                    
                    current_tick = time.time()
                    total_recorded_time += (current_tick - last_tick)
                    last_tick = current_tick
                    
                    print(f"Collected: {total_recorded_time:.1f} / {DURATION_SEC} sec. (Packets: {len(dataset_buffer)})", end='\r')
                    await asyncio.sleep(0.5)

                if client.is_connected and total_recorded_time >= DURATION_SEC:
                    await client.stop_notify(RAWDATA_UUID)
                    print("\n\nData collected!")
                    break 
                    
        except Exception as e:
            print(f"\n[BLE Error] Connection error: {e}")
            await asyncio.sleep(2)

    if len(dataset_buffer) > 0:
        print(f"\nWriting in {OUTPUT_FILE}")
        with open(OUTPUT_FILE, mode='w', newline='') as f:
            writer = csv.writer(f)
            header = ['timestamp', 'packet_id'] + [f'val_{i}' for i in range(60)]
            writer.writerow(header)
            writer.writerows(dataset_buffer)
    else:
        print("Empty data")

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nKeyboard interrupt")